/* SPDX-License-Identifier: Apache-2.0 */
/*
 * State channels. The protocol is spec/state-channel.md (section numbers
 * below refer to it); the interface is spec/c-api.md.
 */
#include <psmsgr/state.h>

#include "internal.h"
#include "layout.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/time_types.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define READ_RETRIES   64
#define ATTACH_RETRIES 2

void (*psmi_test_lock_opened)(void);
bool psmi_test_skip_seq_recheck;

/* ---- small helpers ------------------------------------------------------- */

static uint32_t load_acquire(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* The seqlock fences (§5.4). ThreadSanitizer does not model standalone
 * fences, and GCC warns about that (-Wtsan). The only accesses they order are
 * the racy copies, which TSan cannot see anyway (each handle maps the file
 * separately); everything else synchronizes through the atomics themselves. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#endif
static inline void seq_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void seq_fence_acquire(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

/* Closes fd without clobbering errno. */
static void close_keep_errno(int fd)
{
    int e = errno;
    (void)close(fd);
    errno = e;
}

/* 0 is never a valid generation (§6.5). */
static uint32_t next_generation(uint32_t gen)
{
    return gen == UINT32_MAX ? 1 : gen + 1;
}

/* Shared (not _PRIVATE) futex: waiters and the writer map the same file. */
static void futex_wake_all(uint32_t *addr)
{
    (void)syscall(PSMI_SYS_FUTEX, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

/* Relative timeout, measured on CLOCK_MONOTONIC. Works on read-only mappings. */
static int futex_wait(const uint32_t *addr, uint32_t expected, uint64_t timeout_ns)
{
    struct __kernel_timespec ts = {
        .tv_sec = (__kernel_time64_t)(timeout_ns / 1000000000u),
        .tv_nsec = (long long)(timeout_ns % 1000000000u),
    };
    return (int)syscall(PSMI_SYS_FUTEX, addr, FUTEX_WAIT, expected, &ts, NULL, 0);
}

/* §2: [A-Za-z0-9_-][A-Za-z0-9_.-]{0,63} */
static bool name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.')
        return false;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        if (i == PSMSGR_NAME_MAX)
            return false;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

/* ---- paths ---------------------------------------------------------------- */

/* §2. The directory is resolved once, when a handle opens. */
typedef struct paths {
    char *dir;
    char *data;            /* <dir>/psmsgr.<name>.state */
    char *lock;            /* <dir>/psmsgr.<name>.lock  */
    const char *data_base; /* psmsgr.<name>.state, inside data */
} paths;

static int paths_init(paths *p, const char *dir, const char *name)
{
    if (dir == NULL) {
        const char *env = getenv("PSMSGR_DIR");
        dir = (env != NULL && env[0] != '\0') ? env : "/dev/shm";
    }
    size_t dlen = strlen(dir), nlen = strlen(name);
    size_t data_len = dlen + sizeof "/psmsgr." - 1 + nlen + sizeof ".state" - 1;
    size_t lock_len = dlen + sizeof "/psmsgr." - 1 + nlen + sizeof ".lock" - 1;
    char *buf = malloc(dlen + 1 + data_len + 1 + lock_len + 1);
    if (buf == NULL)
        return PSMSGR_E_SYS;
    p->dir = buf;
    memcpy(p->dir, dir, dlen + 1);
    p->data = p->dir + dlen + 1;
    memcpy(p->data, dir, dlen);
    memcpy(p->data + dlen, "/psmsgr.", 8);
    memcpy(p->data + dlen + 8, name, nlen);
    memcpy(p->data + dlen + 8 + nlen, ".state", sizeof ".state");
    p->lock = p->data + data_len + 1;
    memcpy(p->lock, p->data, dlen + 8 + nlen);
    memcpy(p->lock + dlen + 8 + nlen, ".lock", sizeof ".lock");
    p->data_base = p->data + dlen + 1;
    return PSMSGR_OK;
}

static void paths_free(paths *p)
{
    free(p->dir);
}

/* ---- header validation (§6.1) --------------------------------------------- */

/* `h` is a private copy, so the values checked are the values used. */
static bool header_valid(const psmi_header *h, uint64_t file_bytes)
{
    return h->magic == PSMI_MAGIC && h->version_major == PSMI_VERSION_MAJOR &&
           h->header_size == PSMI_HEADER_SIZE && h->slot_header_size == PSMI_SLOT_HEADER_SIZE &&
           h->slot_count >= PSMI_MIN_SLOTS && h->slot_count <= PSMI_MAX_SLOTS &&
           h->capacity <= PSMSGR_STATE_MAX_CAPACITY &&
           h->slot_stride == psmi_slot_stride(h->capacity) &&
           psmi_file_size(h->slot_count, h->slot_stride) <= file_bytes;
}

/* Reads the header of an open data file into `h`. Returns OK, FORMAT (not a
 * regular file, too short) or SYS. */
static int read_header(int fd, struct stat *st, psmi_header *h)
{
    if (fstat(fd, st) != 0)
        return PSMSGR_E_SYS;
    if (!S_ISREG(st->st_mode) || st->st_size < (off_t)sizeof *h)
        return PSMSGR_E_FORMAT;
    ssize_t n = pread(fd, h, sizeof *h, 0);
    if (n < 0)
        return PSMSGR_E_SYS;
    return n == (ssize_t)sizeof *h ? PSMSGR_OK : PSMSGR_E_FORMAT;
}

/* §5.2 step 4 / §7: tells attached readers and waiters to reattach. */
static void retire(psmi_header *h)
{
    __atomic_fetch_or(&h->state, PSMI_STATE_RETIRED, __ATOMIC_RELEASE);
    __atomic_fetch_add(&h->notify, 1, __ATOMIC_RELEASE);
    futex_wake_all(&h->notify);
}

/* ---- writer lock (§4) ----------------------------------------------------- */

static bool same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* True if `path` still names the inode open as `fd`. On error returns false
 * with *err set to PSMSGR_E_SYS; a missing path is not an error. */
static bool names_inode(const char *path, int fd, int *err)
{
    struct stat fs, ps;
    *err = PSMSGR_OK;
    if (fstat(fd, &fs) != 0) {
        *err = PSMSGR_E_SYS;
        return false;
    }
    if (lstat(path, &ps) != 0) {
        if (errno != ENOENT)
            *err = PSMSGR_E_SYS;
        return false;
    }
    return same_file(&fs, &ps);
}

/* Opens or creates the lock file, takes the OFD write lock and verifies that
 * the path still names the locked inode, starting over if it does not. */
static int lock_channel(const char *path, mode_t mode, int *out)
{
    for (;;) {
        /* O_EXCL first so the mode can be set exactly; opening an existing
         * file without O_CREAT also sidesteps fs.protected_regular. */
        bool created = true;
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno == EEXIST) {
            created = false;
            fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0 && errno == ENOENT)
                continue; /* unlinked in between */
        }
        if (fd < 0)
            return PSMSGR_E_SYS;
        if (created && fchmod(fd, mode) != 0) {
            close_keep_errno(fd);
            return PSMSGR_E_SYS;
        }

        if (psmi_test_lock_opened != NULL)
            psmi_test_lock_opened();

        int err;
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        if (fcntl(fd, F_OFD_SETLK, &fl) != 0) {
            if (errno != EAGAIN && errno != EACCES) {
                close_keep_errno(fd);
                return PSMSGR_E_SYS;
            }
            /* Held by someone. If the path moved on (an unlink holding the
             * old inode), the holder is not a writer of this channel. */
            bool current = names_inode(path, fd, &err);
            close_keep_errno(fd);
            if (err != PSMSGR_OK)
                return err;
            if (current)
                return PSMSGR_E_WRITER_EXISTS;
            continue;
        }
        if (names_inode(path, fd, &err)) {
            *out = fd;
            return PSMSGR_OK;
        }
        close_keep_errno(fd);
        if (err != PSMSGR_OK)
            return err;
    }
}

/* §5.1 step 3. Only exact psmsgr.<name>.state.tmp.XXXXXX names: a longer
 * suffix belongs to another channel (e.g. one named "<name>.state.tmp.a"). */
static void remove_stale_tmp(const paths *p)
{
    DIR *d = opendir(p->dir);
    if (d == NULL)
        return; /* best effort */
    size_t blen = strlen(p->data_base);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (strncmp(n, p->data_base, blen) != 0 || strncmp(n + blen, ".tmp.", 5) != 0)
            continue;
        const char *suffix = n + blen + 5;
        if (strlen(suffix) == 6 && strchr(suffix, '.') == NULL)
            (void)unlinkat(dirfd(d), n, 0);
    }
    (void)closedir(d);
}

/* ---- options -------------------------------------------------------------- */

void psmsgr_state_options_init_sized(psmsgr_state_options *opt, uint32_t size)
{
    if (opt == NULL || size < sizeof opt->struct_size)
        return;
    const psmsgr_state_options d = {
        .struct_size = size,
        .capacity = 0,
        .slot_count = PSMSGR_STATE_DEFAULT_SLOTS,
        .mode = 0644,
    };
    if (size <= sizeof d) {
        memcpy(opt, &d, size);
    } else { /* a newer caller: fields this library doesn't know read as 0 */
        memcpy(opt, &d, sizeof d);
        memset((unsigned char *)opt + sizeof d, 0, size - sizeof d);
    }
}

#define OPT_HAS(opt, field) \
    ((opt)->struct_size >= offsetof(psmsgr_state_options, field) + sizeof(opt)->field)

/* Reads the fields the caller's struct_size covers; NULL means defaults. */
static int load_options(const psmsgr_state_options *opt, psmsgr_state_options *o)
{
    psmsgr_state_options_init(o);
    if (opt != NULL) {
        if (opt->struct_size == 0)
            return PSMSGR_E_INVAL; /* not initialized */
        if (OPT_HAS(opt, capacity))
            o->capacity = opt->capacity;
        if (OPT_HAS(opt, slot_count))
            o->slot_count = opt->slot_count;
        if (OPT_HAS(opt, payload_type))
            o->payload_type = opt->payload_type;
        if (OPT_HAS(opt, mode))
            o->mode = opt->mode;
        if (OPT_HAS(opt, flags))
            o->flags = opt->flags;
        if (OPT_HAS(opt, dir))
            o->dir = opt->dir;
    }
    if (o->capacity > PSMSGR_STATE_MAX_CAPACITY || o->slot_count < PSMI_MIN_SLOTS ||
        o->slot_count > PSMI_MAX_SLOTS || (o->mode & ~07777u) != 0 ||
        (o->flags & ~(uint32_t)(PSMSGR_STATE_RECREATE | PSMSGR_STATE_NO_NOTIFY)) != 0 ||
        (o->dir != NULL && o->dir[0] == '\0'))
        return PSMSGR_E_INVAL;
    return PSMSGR_OK;
}

/* ---- writer ---------------------------------------------------------------- */

struct psmsgr_state_writer {
    psmi_header *hdr;
    size_t map_size;
    uint32_t slot_count;
    uint32_t slot_stride;
    uint32_t capacity;
    bool notify;
    uint32_t latest;    /* W.latest: last committed slot, or NONE */
    uint32_t gen;       /* generation of the next publish */
    uint32_t open_slot; /* slot of an open begin, or NONE */
    uint32_t open_seq;  /* its (even) seq before the begin */
    int lock_fd;
};

static psmi_slot *writer_slot(const psmsgr_state_writer *w, uint32_t i)
{
    return (psmi_slot *)((unsigned char *)w->hdr + PSMI_HEADER_SIZE + (size_t)i * w->slot_stride);
}

static uint32_t config_flags_of(const psmsgr_state_options *o)
{
    return (o->flags & PSMSGR_STATE_NO_NOTIFY) ? PSMI_CONFIG_NO_NOTIFY : 0;
}

/* First generation of a file with no value yet (§5.2). Random, so that a
 * reader's last generation from a file this one replaced (or from before an
 * unlink) is unlikely to match it. Needs no cryptographic quality. */
static uint32_t random_generation(void)
{
    uint32_t g;
    if (getrandom(&g, sizeof g, GRND_INSECURE) != (ssize_t)sizeof g) {
        /* Kernels before 5.6 lack GRND_INSECURE: mix the clocks and the pid. */
        uint64_t x = psmi_clock_ns(CLOCK_MONOTONIC) ^ (psmi_clock_ns(CLOCK_REALTIME) << 17) ^
                     (uint64_t)getpid();
        x ^= x >> 33;
        x *= UINT64_C(0xff51afd7ed558ccd);
        x ^= x >> 33;
        g = (uint32_t)x;
    }
    return g != 0 ? g : 1;
}

/* Generation following the latest value of a mapped file; `stride` and
 * `latest` have been validated. */
static uint32_t carried_generation(const psmi_header *map, uint32_t stride, uint32_t latest)
{
    if (latest == PSMI_LATEST_NONE)
        return random_generation();
    const psmi_slot *s = (const psmi_slot *)((const unsigned char *)map + PSMI_HEADER_SIZE +
                                             (size_t)latest * stride);
    return next_generation(s->generation);
}

/* `geom` is the validated (or just written) header copy. */
static void writer_attach(psmsgr_state_writer *w, psmi_header *map, size_t map_size,
                          const psmi_header *geom, uint32_t latest, uint32_t gen)
{
    w->hdr = map;
    w->map_size = map_size;
    w->slot_count = geom->slot_count;
    w->slot_stride = geom->slot_stride;
    w->capacity = geom->capacity;
    w->notify = (geom->config_flags & PSMI_CONFIG_NO_NOTIFY) == 0;
    w->latest = latest;
    w->gen = gen;
    w->open_slot = PSMI_LATEST_NONE;
}

/* §5.2: builds a complete file under a temporary name and renames it into
 * place, so readers only ever open initialized files. Retires `old` (a
 * mapping of the replaced file's header, may be NULL) on success. */
static int create_file(psmsgr_state_writer *w, const psmsgr_state_options *o, const paths *p,
                       psmi_header *old, uint32_t gen)
{
    size_t tlen = strlen(p->data) + sizeof ".tmp.XXXXXX";
    char *tmp = malloc(tlen);
    if (tmp == NULL)
        return PSMSGR_E_SYS;
    memcpy(tmp, p->data, tlen - sizeof ".tmp.XXXXXX");
    memcpy(tmp + tlen - sizeof ".tmp.XXXXXX", ".tmp.XXXXXX", sizeof ".tmp.XXXXXX");

    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) {
        free(tmp);
        return PSMSGR_E_SYS;
    }

    uint32_t stride = psmi_slot_stride(o->capacity);
    size_t size = (size_t)psmi_file_size(o->slot_count, stride);
    void *map = MAP_FAILED;
    int e;
    if (fchmod(fd, (mode_t)o->mode) != 0)
        goto fail;
    /* Mandatory: a store to an unbacked tmpfs page on a full tmpfs raises
     * SIGBUS in every process mapping the file. */
    if ((e = posix_fallocate(fd, 0, (off_t)size)) != 0) {
        errno = e;
        goto fail;
    }
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (map == MAP_FAILED)
        goto fail;

    /* The file is fresh and therefore zero: slots, reserved bytes, state
     * and notify need no stores. */
    psmi_header *h = map;
    h->magic = PSMI_MAGIC;
    h->version_major = PSMI_VERSION_MAJOR;
    h->version_minor = PSMI_VERSION_MINOR;
    h->header_size = PSMI_HEADER_SIZE;
    h->slot_header_size = PSMI_SLOT_HEADER_SIZE;
    h->slot_count = o->slot_count;
    h->slot_stride = stride;
    h->capacity = o->capacity;
    h->payload_type = o->payload_type;
    h->config_flags = config_flags_of(o);
    h->latest = PSMI_LATEST_NONE;
    h->created_realtime_ns = psmi_clock_ns(CLOCK_REALTIME);

    if (rename(tmp, p->data) != 0)
        goto fail;
    (void)close(fd);
    free(tmp);

    if (old != NULL)
        retire(old);
    writer_attach(w, h, size, h, PSMI_LATEST_NONE, gen);
    return PSMSGR_OK;

fail:
    e = errno;
    if (map != MAP_FAILED)
        (void)munmap(map, size);
    (void)close(fd);
    (void)unlink(tmp);
    free(tmp);
    errno = e;
    return PSMSGR_E_SYS;
}

/* §5.1 step 4: create, reuse, recreate or refuse. */
static int open_data(psmsgr_state_writer *w, const psmsgr_state_options *o, const paths *p)
{
    int fd = open(p->data, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? create_file(w, o, p, NULL, random_generation()) : PSMSGR_E_SYS;

    struct stat st;
    psmi_header h;
    int rc = read_header(fd, &st, &h);
    if (rc == PSMSGR_E_SYS) {
        close_keep_errno(fd);
        return rc;
    }
    bool valid = rc == PSMSGR_OK && header_valid(&h, (uint64_t)st.st_size);

    /* Map the whole file if it is valid, else just the header if it has our
     * magic (so it can be retired), else nothing. */
    size_t map_size = valid ? (size_t)psmi_file_size(h.slot_count, h.slot_stride)
                      : (rc == PSMSGR_OK && h.magic == PSMI_MAGIC) ? sizeof h
                                                                   : 0;
    psmi_header *old = NULL;
    if (map_size != 0) {
        void *m = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close_keep_errno(fd);
            return PSMSGR_E_SYS;
        }
        old = m;
    }
    (void)close(fd);

    uint32_t latest = valid ? load_acquire(&old->latest) : PSMI_LATEST_NONE;
    if (valid && !psmi_latest_valid(latest, h.slot_count))
        valid = false; /* corrupt dynamic state */
    if (latest != PSMI_LATEST_NONE)
        latest = psmi_latest_slot(latest);

    bool recreate = (o->flags & PSMSGR_STATE_RECREATE) != 0;
    uint32_t gen = valid ? carried_generation(old, h.slot_stride, latest) : random_generation();
    if (!valid) {
        rc = recreate ? create_file(w, o, p, old, gen) : PSMSGR_E_FORMAT;
    } else if ((load_acquire(&old->state) & PSMI_STATE_RETIRED) != 0) {
        /* Left retired by an interrupted unlink: as good as absent. */
        rc = create_file(w, o, p, old, gen);
    } else if (h.capacity != o->capacity || h.slot_count != o->slot_count ||
               h.payload_type != o->payload_type || h.config_flags != config_flags_of(o)) {
        rc = recreate ? create_file(w, o, p, old, gen) : PSMSGR_E_MISMATCH;
    } else if (h.version_minor != PSMI_VERSION_MINOR) {
        /* Replaced automatically; readers follow the retire. */
        rc = create_file(w, o, p, old, gen);
    } else {
        /* §5.3 reuse. A slot left odd by a crashed publish stays odd: its
         * payload may be partial. It is the slot the next publish writes. */
        writer_attach(w, old, map_size, &h, latest, gen);
        return PSMSGR_OK;
    }
    if (old != NULL)
        (void)munmap(old, map_size);
    return rc;
}

int psmsgr_state_writer_open(const char *name, const psmsgr_state_options *opt,
                             psmsgr_state_writer **out)
{
    if (out == NULL)
        return PSMSGR_E_INVAL;
    *out = NULL;
    psmsgr_state_options o;
    int rc = load_options(opt, &o);
    if (rc != PSMSGR_OK)
        return rc;
    if (!name_valid(name))
        return PSMSGR_E_INVAL;

    paths p;
    if ((rc = paths_init(&p, o.dir, name)) != PSMSGR_OK)
        return rc;
    psmsgr_state_writer *w = calloc(1, sizeof *w);
    if (w == NULL) {
        paths_free(&p);
        return PSMSGR_E_SYS;
    }

    rc = lock_channel(p.lock, (mode_t)o.mode, &w->lock_fd);
    if (rc == PSMSGR_OK) {
        remove_stale_tmp(&p);
        rc = open_data(w, &o, &p);
        if (rc != PSMSGR_OK)
            close_keep_errno(w->lock_fd);
    }
    paths_free(&p);
    if (rc != PSMSGR_OK) {
        int e = errno;
        free(w);
        errno = e;
        return rc;
    }
    w->hdr->writer_pid = (uint32_t)getpid();
    *out = w;
    return PSMSGR_OK;
}

void psmsgr_state_writer_close(psmsgr_state_writer *w)
{
    if (w == NULL)
        return;
    /* An open begin is aborted implicitly: its slot stays odd. */
    (void)munmap(w->hdr, w->map_size);
    (void)close(w->lock_fd); /* releases the OFD lock */
    free(w);
}

uint32_t psmsgr_state_writer_capacity(const psmsgr_state_writer *w)
{
    return w != NULL ? w->capacity : 0;
}

void psmi_writer_set_generation(psmsgr_state_writer *w, uint32_t next)
{
    w->gen = next;
}

/* §5.4 up to and including the release fence. The target is never `latest`.
 * A slot that is already odd (an aborted or crashed publish) stays odd. */
static unsigned char *begin_slot(psmsgr_state_writer *w)
{
    uint32_t i = w->latest == PSMI_LATEST_NONE ? 0 : (w->latest + 1) % w->slot_count;
    psmi_slot *s = writer_slot(w, i);
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED) & ~1u;
    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    seq_fence_release();
    w->open_slot = i;
    w->open_seq = q;
    return (unsigned char *)s + PSMI_SLOT_HEADER_SIZE;
}

/* §5.4 from the slot header fields on. */
static uint32_t commit_slot(psmsgr_state_writer *w, uint32_t len)
{
    uint32_t i = w->open_slot;
    psmi_slot *s = writer_slot(w, i);
    uint32_t gen = w->gen;
    s->generation = gen;
    s->length = len;
    s->timestamp_ns = psmi_clock_ns(CLOCK_MONOTONIC);
    __atomic_store_n(&s->seq, w->open_seq + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&w->hdr->latest, psmi_latest(i, w->open_seq + 2), __ATOMIC_RELEASE);
    w->latest = i;
    w->open_slot = PSMI_LATEST_NONE;
    if (w->notify) {
        __atomic_fetch_add(&w->hdr->notify, 1, __ATOMIC_RELEASE);
        futex_wake_all(&w->hdr->notify);
    }
    w->gen = next_generation(gen);
    return gen;
}

int psmsgr_state_publish(psmsgr_state_writer *w, const void *data, uint32_t len,
                         uint32_t *generation)
{
    if (w == NULL || (data == NULL && len != 0))
        return PSMSGR_E_INVAL;
    if (w->open_slot != PSMI_LATEST_NONE)
        return PSMSGR_E_STATE;
    if (len > w->capacity)
        return PSMSGR_E_TOOBIG;
    psmi_seq_copy(begin_slot(w), data, len);
    uint32_t gen = commit_slot(w, len);
    if (generation != NULL)
        *generation = gen;
    return PSMSGR_OK;
}

int psmsgr_state_begin(psmsgr_state_writer *w, void **buf)
{
    if (w == NULL || buf == NULL)
        return PSMSGR_E_INVAL;
    if (w->open_slot != PSMI_LATEST_NONE)
        return PSMSGR_E_STATE;
    *buf = begin_slot(w);
    return PSMSGR_OK;
}

int psmsgr_state_commit(psmsgr_state_writer *w, uint32_t len, uint32_t *generation)
{
    if (w == NULL)
        return PSMSGR_E_INVAL;
    if (w->open_slot == PSMI_LATEST_NONE)
        return PSMSGR_E_STATE;
    if (len > w->capacity)
        return PSMSGR_E_TOOBIG; /* the begin stays open */
    uint32_t gen = commit_slot(w, len);
    if (generation != NULL)
        *generation = gen;
    return PSMSGR_OK;
}

int psmsgr_state_abort(psmsgr_state_writer *w)
{
    if (w == NULL)
        return PSMSGR_E_INVAL;
    if (w->open_slot == PSMI_LATEST_NONE)
        return PSMSGR_E_STATE;
    /* The slot stays odd: the caller may have overwritten part of its
     * payload, so it must not become readable with its old header. The next
     * begin picks the same slot. */
    w->open_slot = PSMI_LATEST_NONE;
    return PSMSGR_OK;
}

/* ---- reader ---------------------------------------------------------------- */

struct psmsgr_state_reader {
    const psmi_header *hdr; /* NULL while unattached */
    size_t map_size;
    uint32_t slot_count;
    uint32_t slot_stride;
    uint32_t capacity;
    uint32_t payload_type;
    uint32_t config_flags;
    bool announce; /* next info carries PSMSGR_INFO_ATTACHED */
    dev_t dev;
    ino_t ino;
    paths paths;
};

int psmsgr_state_reader_open(const char *name, const char *dir, psmsgr_state_reader **out)
{
    if (out == NULL)
        return PSMSGR_E_INVAL;
    *out = NULL;
    if (!name_valid(name) || (dir != NULL && dir[0] == '\0'))
        return PSMSGR_E_INVAL;
    psmsgr_state_reader *r = calloc(1, sizeof *r);
    if (r == NULL)
        return PSMSGR_E_SYS;
    if (paths_init(&r->paths, dir, name) != PSMSGR_OK) {
        free(r);
        errno = ENOMEM;
        return PSMSGR_E_SYS;
    }
    *out = r;
    return PSMSGR_OK;
}

static void detach(psmsgr_state_reader *r)
{
    (void)munmap((void *)(uintptr_t)r->hdr, r->map_size);
    r->hdr = NULL;
}

void psmsgr_state_reader_close(psmsgr_state_reader *r)
{
    if (r == NULL)
        return;
    if (r->hdr != NULL)
        detach(r);
    paths_free(&r->paths);
    free(r);
}

/* §6.1. An attached reader holds no file descriptor. */
static int attach(psmsgr_state_reader *r)
{
    int fd = open(r->paths.data, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? PSMSGR_E_NODATA : PSMSGR_E_SYS;

    struct stat st;
    psmi_header h;
    int rc = read_header(fd, &st, &h);
    if (rc == PSMSGR_OK && !header_valid(&h, (uint64_t)st.st_size))
        rc = PSMSGR_E_FORMAT;
    if (rc == PSMSGR_OK) {
        size_t size = (size_t)psmi_file_size(h.slot_count, h.slot_stride);
        void *m = mmap(NULL, size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
        if (m == MAP_FAILED) {
            rc = PSMSGR_E_SYS;
        } else {
            r->hdr = m;
            r->map_size = size;
            r->slot_count = h.slot_count;
            r->slot_stride = h.slot_stride;
            r->capacity = h.capacity;
            r->payload_type = h.payload_type;
            r->config_flags = h.config_flags;
            r->dev = st.st_dev;
            r->ino = st.st_ino;
            r->announce = true;
        }
    }
    close_keep_errno(fd);
    return rc;
}

static bool retired(const psmsgr_state_reader *r)
{
    return (load_acquire(&r->hdr->state) & PSMI_STATE_RETIRED) != 0;
}

/* §6.2 retire check, plus a lazy attach while unattached. A retired file
 * still found at the path (a replacement or unlink in progress, or an
 * interrupted unlink) counts as absent. */
static int reader_sync(psmsgr_state_reader *r)
{
    if (r->hdr != NULL) {
        if (!retired(r))
            return PSMSGR_OK;
        detach(r);
    }
    for (int i = 0; i < ATTACH_RETRIES; ++i) {
        int rc = attach(r);
        if (rc != PSMSGR_OK)
            return rc;
        if (!retired(r))
            return PSMSGR_OK;
        detach(r);
    }
    return PSMSGR_E_NODATA;
}

int psmi_reader_identity_check(psmsgr_state_reader *r)
{
    if (r->hdr == NULL)
        return PSMSGR_OK;
    struct stat st;
    if (lstat(r->paths.data, &st) == 0) {
        if (st.st_dev == r->dev && st.st_ino == r->ino)
            return PSMSGR_OK;
    } else if (errno != ENOENT) {
        return PSMSGR_E_SYS;
    }
    detach(r);
    return PSMSGR_OK;
}

/* §6.3 / §6.4 on an attached reader. With copy == false this is peek. */
static int read_latest(const psmsgr_state_reader *r, void *buf, uint32_t size, bool copy,
                       psmsgr_state_info *info)
{
    const unsigned char *slots = (const unsigned char *)r->hdr + PSMI_HEADER_SIZE;
    for (int attempt = 0; attempt < READ_RETRIES; ++attempt) {
        uint32_t latest = load_acquire(&r->hdr->latest);
        if (latest == PSMI_LATEST_NONE)
            return PSMSGR_E_NODATA;
        if (!psmi_latest_valid(latest, r->slot_count))
            return PSMSGR_E_FORMAT;
        uint32_t i = psmi_latest_slot(latest);
        const psmi_slot *s = (const psmi_slot *)(slots + (size_t)i * r->slot_stride);

        /* Only the version `latest` published: a stale `latest` may name a
         * slot the writer has since committed again but not yet published,
         * and reading that would let the next read go back in time. */
        uint32_t q1 = load_acquire(&s->seq);
        if ((q1 & 1u) == 0 && psmi_latest(i, q1) == latest) {
            psmi_slot m;
            psmi_seq_copy(&m, s, offsetof(psmi_slot, reserved));
            bool fits = m.length <= size;
            if (copy && fits && m.length <= r->capacity)
                psmi_seq_copy(buf, (const unsigned char *)s + PSMI_SLOT_HEADER_SIZE, m.length);
            seq_fence_acquire();
            if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == q1 || psmi_test_skip_seq_recheck) {
                if (m.length > r->capacity)
                    return PSMSGR_E_FORMAT;
                *info = (psmsgr_state_info){
                    .generation = m.generation,
                    .length = m.length,
                    .timestamp_ns = m.timestamp_ns,
                };
                return (!copy || fits) ? PSMSGR_OK : PSMSGR_E_TOOSMALL;
            }
        }
        if (attempt >= 3)
            (void)sched_yield(); /* single core: let a preempted writer finish */
    }
    return PSMSGR_E_BUSY;
}

/* PSMSGR_INFO_ATTACHED: once per attach, on the first result handed out. */
static void announce(psmsgr_state_reader *r, psmsgr_state_info *info)
{
    if (r->announce) {
        info->flags |= PSMSGR_INFO_ATTACHED;
        r->announce = false;
    }
}

int psmsgr_state_read(psmsgr_state_reader *r, void *buf, uint32_t size, psmsgr_state_info *info)
{
    if (r == NULL || info == NULL || (buf == NULL && size != 0))
        return PSMSGR_E_INVAL;
    int rc = reader_sync(r);
    if (rc != PSMSGR_OK)
        return rc;
    rc = read_latest(r, buf, size, true, info);
    if (rc == PSMSGR_OK || rc == PSMSGR_E_TOOSMALL)
        announce(r, info);
    return rc;
}

int psmsgr_state_peek(psmsgr_state_reader *r, psmsgr_state_info *info)
{
    if (r == NULL || info == NULL)
        return PSMSGR_E_INVAL;
    int rc = reader_sync(r);
    if (rc != PSMSGR_OK)
        return rc;
    rc = read_latest(r, NULL, 0, false, info);
    if (rc == PSMSGR_OK)
        announce(r, info);
    return rc;
}

int psmsgr_state_describe_sized(psmsgr_state_reader *r, psmsgr_state_desc *desc, uint32_t size)
{
    if (r == NULL || desc == NULL || size == 0)
        return PSMSGR_E_INVAL;
    int rc = reader_sync(r);
    if (rc != PSMSGR_OK)
        return rc;
    const psmsgr_state_desc d = {
        .capacity = r->capacity,
        .slot_count = r->slot_count,
        .payload_type = r->payload_type,
        .flags = (r->config_flags & PSMI_CONFIG_NO_NOTIFY) ? PSMSGR_STATE_NO_NOTIFY : 0,
    };
    if (size <= sizeof d) {
        memcpy(desc, &d, size);
    } else { /* a newer caller: fields this library doesn't know read as 0 */
        memcpy(desc, &d, sizeof d);
        memset((unsigned char *)desc + sizeof d, 0, size - sizeof d);
    }
    return PSMSGR_OK;
}

int psmi_reader_wait_step(psmsgr_state_reader *r, uint32_t last_generation, uint32_t *generation,
                          const uint32_t **addr, uint32_t *expected)
{
    for (;;) {
        int rc = reader_sync(r);
        if (rc == PSMSGR_E_NODATA)
            return PSMI_STEP_UNATTACHED;
        if (rc != PSMSGR_OK)
            return rc;
        if (r->config_flags & PSMI_CONFIG_NO_NOTIFY)
            return PSMSGR_E_NOTSUP;
        /* Load notify BEFORE the check: a publish in between changes it, and
         * the futex wait then returns at once instead of losing the wake-up. */
        uint32_t n = load_acquire(&r->hdr->notify);
        if (retired(r))
            continue;
        psmsgr_state_info info;
        rc = read_latest(r, NULL, 0, false, &info);
        if (rc == PSMSGR_OK && info.generation != last_generation) {
            *generation = info.generation;
            return PSMSGR_OK;
        }
        if (rc == PSMSGR_E_FORMAT)
            return rc;
        /* NODATA and BUSY count as unchanged. */
        *addr = &r->hdr->notify;
        *expected = n;
        return PSMI_STEP_ARMED;
    }
}

/* §6.6 */
int psmsgr_state_wait(psmsgr_state_reader *r, uint32_t last_generation, int32_t timeout_ms)
{
    if (r == NULL)
        return PSMSGR_E_INVAL;
    bool infinite = timeout_ms < 0;
    uint64_t deadline =
        infinite ? UINT64_MAX : psmi_clock_ns(CLOCK_MONOTONIC) + (uint64_t)timeout_ms * NS_PER_MS;
    for (;;) {
        uint32_t generation, n;
        const uint32_t *notify;
        int rc = psmi_reader_wait_step(r, last_generation, &generation, &notify, &n);
        if (rc <= PSMSGR_OK) /* changed, or an error; the PSMI_STEP_* are > 0 */
            return rc;

        uint64_t now = psmi_clock_ns(CLOCK_MONOTONIC);
        if (now >= deadline)
            return PSMSGR_E_TIMEOUT;
        if (rc == PSMI_STEP_UNATTACHED) { /* poll the attach */
            uint64_t ns = deadline - now < UNATTACHED_POLL_NS ? deadline - now : UNATTACHED_POLL_NS;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ns };
            if (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL) == EINTR)
                return PSMSGR_E_INTR;
            continue;
        }

        uint64_t slice = deadline - now < WAIT_SLICE_NS ? deadline - now : WAIT_SLICE_NS;
        if (futex_wait(notify, n, slice) != 0) {
            if (errno == EINTR)
                return PSMSGR_E_INTR;
            if (errno == ETIMEDOUT) {
                if ((rc = psmi_reader_identity_check(r)) != PSMSGR_OK)
                    return rc;
            } else if (errno != EAGAIN) {
                return PSMSGR_E_SYS; /* e.g. ENOSYS under seccomp: never spin */
            }
        }
    }
}

int psmsgr_state_writer_alive(psmsgr_state_reader *r)
{
    if (r == NULL)
        return PSMSGR_E_INVAL;
    int rc = psmi_reader_identity_check(r);
    if (rc != PSMSGR_OK)
        return rc;
    (void)reader_sync(r); /* reattach if possible; liveness does not depend on it */

    int fd = open(r->paths.lock, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : PSMSGR_E_SYS;
    /* Reports a conflicting lock without taking one (§4). */
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    rc = fcntl(fd, F_OFD_GETLK, &fl);
    close_keep_errno(fd);
    if (rc != 0)
        return PSMSGR_E_SYS;
    return fl.l_type != F_UNLCK;
}

/* ---- management ------------------------------------------------------------ */

/* §7 */
int psmsgr_state_unlink(const char *name, const char *dir)
{
    if (!name_valid(name) || (dir != NULL && dir[0] == '\0'))
        return PSMSGR_E_INVAL;
    paths p;
    int rc = paths_init(&p, dir, name);
    if (rc != PSMSGR_OK)
        return rc;
    int lock_fd;
    if ((rc = lock_channel(p.lock, 0600, &lock_fd)) != PSMSGR_OK) {
        paths_free(&p);
        return rc;
    }
    remove_stale_tmp(&p);

    rc = PSMSGR_E_NODATA;
    int fd = open(p.data, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        struct stat st;
        psmi_header h, *old = NULL;
        if (read_header(fd, &st, &h) == PSMSGR_OK && h.magic == PSMI_MAGIC) {
            void *m = mmap(NULL, sizeof h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED)
                old = m;
        }
        (void)close(fd);
        /* Unlink first, then retire: readers that wake up and reattach find
         * the path already gone. */
        if (unlink(p.data) == 0)
            rc = PSMSGR_OK;
        else if (errno != ENOENT)
            rc = PSMSGR_E_SYS;
        if (old != NULL) {
            if (rc == PSMSGR_OK)
                retire(old);
            (void)munmap(old, sizeof h);
        }
    } else if (errno != ENOENT) {
        rc = PSMSGR_E_SYS;
    }

    /* Remove the lock file while still holding it, so a writer that opened
     * it concurrently fails its identity check and starts over. */
    if (rc != PSMSGR_E_SYS && unlink(p.lock) != 0 && errno != ENOENT)
        rc = PSMSGR_E_SYS;
    close_keep_errno(lock_fd);
    paths_free(&p);
    return rc;
}
