# C API — `libpsmsgr`

Status: **final**. Protocol semantics are defined in
[state-channel.md](state-channel.md); this document defines the interface.

## Conventions

- C11. Headers compile as C++ as well (`extern "C"`).
- Every function that can fail returns `int`: `PSMSGR_OK` (0) or a negative
  `PSMSGR_E_*` code. When the code is `PSMSGR_E_SYS`, `errno` holds the error
  from the failing system call.
- Handles are opaque. A handle is **not** thread-safe: synchronize externally
  or use one handle per thread. Different handles are independent, including
  handles to the same channel. The one exception is a waitset's `add`,
  `remove` and `wake`, which any thread may call.
- Handles are not valid in a `fork()`ed child. All descriptors are
  `O_CLOEXEC`.
- Nothing allocates or takes a lock on the hot path (`publish`,
  `begin`/`commit`, `read`, `peek`). All allocation happens in `*_open`.
- `read` and `peek` make no syscalls while attached. `publish`/`commit`
  make at most two: the notify wake, and `clock_gettime` where the vDSO
  can't serve it (always on the AM335x, about 1.3 µs; see
  state-channel.md §8).
- The library never writes to stdout/stderr, never installs signal handlers,
  never calls `exit`/`abort`, never creates threads, and keeps no global
  mutable state apart from handles.
- Only the `PSMSGR_API` functions are exported (`-fvisibility=hidden`, an
  export macro, and a version script, `src/libpsmsgr.map`, that lists each
  one). Functions released in 1.0 have version `PSMSGR_1`. Functions added
  in 1.x go in a new node `PSMSGR_1.<minor>`, which inherits from the
  previous node: 1.1 added `PSMSGR_1.1`, the waitset. Released nodes never
  change. Removing or changing an exported function is an ABI break and
  bumps the SONAME.
- ABI extensibility: structs passed *in* start with `struct_size`, the size
  of the caller's struct. The library reads only the fields that size covers
  and that it knows. Their `*_init` function is `static inline` in the
  header, so that `struct_size` is the caller's `sizeof`: a newer library
  never writes past the end of an older caller's struct. It forwards to an
  exported `*_init_sized(opt, size)`, which bindings call with the size of
  their own struct.
- Structs the library fills (*out*) and that may grow get the same
  treatment: an exported `*_sized(..., size)` writes only the first `size`
  bytes and zeroes any beyond the library's own struct, and a `static
  inline` wrapper passes the caller's `sizeof`. New fields are appended and
  read as 0 from an older library, so 0 must mean "unknown". This applies to
  `psmsgr_state_desc`. `psmsgr_state_info` is filled on every `read` and
  `peek` and stays fixed at 24 bytes; it grows only through its unused
  `flags` bits and `reserved`.

## `<psmsgr/psmsgr.h>`

```c
/* Library major == SONAME number. The first release is 1.0.0; until then the
 * API/ABI may change freely. */
#define PSMSGR_VERSION_MAJOR 1
#define PSMSGR_VERSION_MINOR 1
#define PSMSGR_VERSION_PATCH 0

/* (major << 16) | (minor << 8) | patch of the loaded library. */
uint32_t    psmsgr_version(void);

/* Static, never NULL. Unknown codes yield "unknown error". */
const char *psmsgr_strerror(int code);

/* CLOCK_MONOTONIC in nanoseconds: the clock used for timestamp_ns. */
uint64_t    psmsgr_now_ns(void);

enum {
    PSMSGR_OK              =   0,
    PSMSGR_E_INVAL         =  -1,  /* bad argument or channel name            */
    PSMSGR_E_SYS           =  -2,  /* system call failed; see errno           */
    PSMSGR_E_NODATA        =  -3,  /* channel absent, nothing published yet, or
                                    * reader not in the waitset              */
    PSMSGR_E_TOOSMALL      =  -4,  /* buffer too small; info->length is valid */
    PSMSGR_E_TOOBIG        =  -5,  /* payload over capacity, or waitset full  */
    PSMSGR_E_BUSY          =  -6,  /* read retries exhausted; transient, retry */
    PSMSGR_E_TIMEOUT       =  -7,
    PSMSGR_E_INTR          =  -8,  /* wait interrupted by a signal            */
    PSMSGR_E_WRITER_EXISTS =  -9,  /* another writer holds the channel        */
    PSMSGR_E_MISMATCH      = -10,  /* existing channel has other geometry     */
    PSMSGR_E_FORMAT        = -11,  /* bad magic/version/size, corrupt file    */
    PSMSGR_E_NOTSUP        = -12,  /* NO_NOTIFY channel; no futex_waitv       */
    PSMSGR_E_STATE         = -13,  /* call not valid now, e.g. commit w/o begin */
};
```

## `<psmsgr/state.h>`

### Types

```c
#define PSMSGR_NAME_MAX            64
#define PSMSGR_STATE_MAX_CAPACITY  (16u << 20)
#define PSMSGR_STATE_DEFAULT_SLOTS 3

typedef struct psmsgr_state_writer psmsgr_state_writer;
typedef struct psmsgr_state_reader psmsgr_state_reader;

enum {
    PSMSGR_STATE_RECREATE  = 1u << 0,  /* replace an incompatible existing channel */
    PSMSGR_STATE_NO_NOTIFY = 1u << 1,  /* no futex wake per publish; wait() -> NOTSUP */
};

typedef struct psmsgr_state_options {
    uint32_t    struct_size;   /* set by psmsgr_state_options_init[_sized] */
    uint32_t    capacity;      /* max payload bytes, 0 .. PSMSGR_STATE_MAX_CAPACITY */
    uint32_t    slot_count;    /* 2 .. 16; default PSMSGR_STATE_DEFAULT_SLOTS */
    uint32_t    payload_type;  /* application tag; default 0 */
    uint32_t    mode;          /* file mode; default 0644 */
    uint32_t    flags;         /* PSMSGR_STATE_* */
    const char *dir;           /* NULL: $PSMSGR_DIR, else /dev/shm */
} psmsgr_state_options;

enum {
    PSMSGR_INFO_ATTACHED = 1u << 0,  /* first result from a newly (re)attached file */
};

/* Result of read/peek. */
typedef struct psmsgr_state_info {
    uint32_t generation;    /* change token, never 0 */
    uint32_t length;        /* payload length */
    uint64_t timestamp_ns;  /* CLOCK_MONOTONIC at publish */
    uint32_t flags;         /* PSMSGR_INFO_* */
    uint32_t reserved;      /* 0 */
} psmsgr_state_info;

/* Constant properties of an attached channel. Filled by
 * psmsgr_state_describe[_sized]; a field the library doesn't know reads as 0. */
typedef struct psmsgr_state_desc {
    uint32_t capacity;
    uint32_t slot_count;
    uint32_t payload_type;
    uint32_t flags;         /* PSMSGR_STATE_NO_NOTIFY if set on the channel */
} psmsgr_state_desc;
```

### Writer

```c
/* Sets the defaults and struct_size = size, writing only the first `size`
 * bytes of *opt (zeroing any beyond the library's own struct). */
void psmsgr_state_options_init_sized(psmsgr_state_options *opt, uint32_t size);

static inline void psmsgr_state_options_init(psmsgr_state_options *opt)
{
    psmsgr_state_options_init_sized(opt, (uint32_t)sizeof *opt);
}

/* Opens or creates the channel and takes the writer lock (state-channel.md §5.1).
 * opt NULL: psmsgr_state_options_init() defaults.
 * Errors: INVAL, WRITER_EXISTS, MISMATCH, FORMAT,
 *         SYS (e.g. ENOENT: dir missing, ENOSPC: tmpfs full, EACCES, ELOOP: symlink). */
int  psmsgr_state_writer_open(const char *name,
                              const psmsgr_state_options *opt,
                              psmsgr_state_writer **out);

/* Aborts an open begin, releases the lock; the channel and its last value
 * remain. NULL is a no-op. */
void psmsgr_state_writer_close(psmsgr_state_writer *w);

/* Copies and publishes a value. generation may be NULL.
 * Errors: TOOBIG, STATE (a begin is open). */
int  psmsgr_state_publish(psmsgr_state_writer *w, const void *data,
                          uint32_t len, uint32_t *generation);

/* Zero-copy publish. *buf points at `capacity` writable bytes (32-byte
 * aligned) until commit/abort. Exactly one of commit/abort must follow.
 * It saves publish's copy only when the value is built in *buf; a value
 * already in a buffer of its own gains nothing over publish. On the
 * BeagleBone Black the saving is negligible up to 256 B and about half at
 * 64 KiB. */
int  psmsgr_state_begin (psmsgr_state_writer *w, void **buf);
int  psmsgr_state_commit(psmsgr_state_writer *w, uint32_t len, uint32_t *generation);
int  psmsgr_state_abort (psmsgr_state_writer *w);

uint32_t psmsgr_state_writer_capacity(const psmsgr_state_writer *w);
```

### Reader

```c
/* Always succeeds for a valid name; attaches lazily (state-channel.md §6.1).
 * dir: NULL -> $PSMSGR_DIR, else /dev/shm. Errors: INVAL, SYS (ENOMEM). */
int  psmsgr_state_reader_open(const char *name, const char *dir,
                              psmsgr_state_reader **out);
void psmsgr_state_reader_close(psmsgr_state_reader *r);

/* Copies the latest value into buf. info must not be NULL.
 * OK | NODATA | TOOSMALL (nothing copied, info->length set) | BUSY | FORMAT
 * | SYS (the channel file cannot be opened, e.g. EACCES, ELOOP). */
int  psmsgr_state_read(psmsgr_state_reader *r, void *buf, uint32_t size,
                       psmsgr_state_info *info);

/* Generation, length and timestamp of the latest value without copying it.
 * No syscalls while attached. OK | NODATA | BUSY | FORMAT | SYS. */
int  psmsgr_state_peek(psmsgr_state_reader *r, psmsgr_state_info *info);

/* Blocks until the generation differs from last_generation (0 = "any value").
 * timeout_ms < 0: infinite, 0: poll once.
 * OK | TIMEOUT | INTR | NOTSUP | FORMAT | SYS (e.g. ENOSYS: futex blocked). */
int  psmsgr_state_wait(psmsgr_state_reader *r, uint32_t last_generation,
                       int32_t timeout_ms);

/* 1 if a writer currently holds the channel, 0 if not, <0 on error.
 * Syscalls; also runs the orphan identity check (state-channel.md §6.2) and
 * reattaches if the file was replaced behind the library's back. */
int  psmsgr_state_writer_alive(psmsgr_state_reader *r);

/* Constant channel properties, writing only the first `size` bytes of *desc
 * (zeroing any beyond the library's own struct).
 * OK | NODATA (not attached) | INVAL (size 0) | FORMAT | SYS. */
int  psmsgr_state_describe_sized(psmsgr_state_reader *r, psmsgr_state_desc *desc,
                                 uint32_t size);

static inline int psmsgr_state_describe(psmsgr_state_reader *r, psmsgr_state_desc *desc)
{
    return psmsgr_state_describe_sized(r, desc, (uint32_t)sizeof *desc);
}
```

### Management

```c
/* Retires and deletes the channel (state-channel.md §7).
 * OK | NODATA (absent) | WRITER_EXISTS | INVAL | SYS. */
int  psmsgr_state_unlink(const char *name, const char *dir);
```

## `<psmsgr/waitset.h>`

Added in 1.1. A waitset lets one thread wait for many readers at once
(state-channel.md §6.7), so that an event loop or an async runtime needs one
blocked thread instead of one per reader. The library creates no thread:
the caller dedicates one to `wait` and hands its events to the loop; other
threads add and remove readers. It needs `futex_waitv` (Linux 5.16). Where
that is missing, `open` returns `NOTSUP` and callers keep a thread per
reader in `psmsgr_state_wait`.

```c
/* Readers per waitset: futex_waitv takes 128 futexes, one is the set's own. */
#define PSMSGR_WAITSET_MAX 127

typedef struct psmsgr_waitset psmsgr_waitset;

/* A registration that finished. Fixed layout: wait fills an array of these. */
typedef struct psmsgr_waitset_event {
    uint64_t token;       /* as passed to psmsgr_waitset_add */
    int32_t  status;      /* OK: the generation differs from last_generation; else what
                           * psmsgr_state_wait would return: NOTSUP | FORMAT | SYS */
    uint32_t generation;  /* status OK: the generation now; else 0 */
    int32_t  sys_errno;   /* status SYS: errno of the failed call; else 0 */
    uint32_t reserved;    /* 0 */
} psmsgr_waitset_event;   /* 24 bytes */

/* Creates an empty waitset. NOTSUP without futex_waitv: Linux < 5.16,
 * qemu-user, or a seccomp filter that returns ENOSYS or EPERM.
 * Errors: INVAL, NOTSUP, SYS (ENOMEM). */
int  psmsgr_waitset_open(psmsgr_waitset **out);

/* Frees the set. Registered readers stay open and are the caller's again. No
 * other call on the set may be in progress. NULL is a no-op. */
void psmsgr_waitset_close(psmsgr_waitset *ws);

/* Registers r until its generation differs from last_generation (0 = "any
 * value"), with psmsgr_state_wait's semantics; token comes back in the event.
 * The set owns r until wait reports it or remove succeeds: until then the
 * caller must not use r in any way, including close. May block while another
 * thread's wait scans the set (attaching channels).
 * OK | INVAL | TOOBIG (PSMSGR_WAITSET_MAX registered) | STATE (r is registered). */
int  psmsgr_waitset_add(psmsgr_waitset *ws, psmsgr_state_reader *r,
                        uint32_t last_generation, uint64_t token);

/* Unregisters r; the caller may close it as soon as this returns, even while
 * another thread is blocked in wait. Blocks until that wait has left the
 * kernel, which it does at once.
 * OK | NODATA (not registered: never added, or already reported) | INVAL. */
int  psmsgr_waitset_remove(psmsgr_waitset *ws, psmsgr_state_reader *r);

/* Blocks until a registered reader finishes or psmsgr_waitset_wake is called.
 * Writes up to cap events and sets *n; reported readers are unregistered and
 * the caller's again, the rest stay registered. One thread at a time.
 * timeout_ms < 0: infinite, 0: check once.
 * OK (*n >= 1, or 0 after a wake) | TIMEOUT | INTR | STATE (another thread is
 * in wait) | INVAL | SYS. *n is 0 unless OK. */
int  psmsgr_waitset_wait(psmsgr_waitset *ws, int32_t timeout_ms,
                         psmsgr_waitset_event *events, uint32_t cap, uint32_t *n);

/* Makes the current wait return, or the next one if none is in progress. Any
 * thread; never blocks. NULL is a no-op. */
void psmsgr_waitset_wake(psmsgr_waitset *ws);
```

- A registration is one-shot: to keep following a reader, add it again with
  the event's `generation`. A reported reader is not touched by the set
  again, so the caller may read or close it while the waiting thread goes
  on.
- A cancellation that races a report: `remove` returns `NODATA` and the
  event is the answer.
- `wake` is how another thread stops the waiting one: set a flag, `wake`,
  join, then `close`. A wake while no `wait` is in progress ends the next
  one.
- The set takes an internal lock (a futex, not `pthread_mutex`). `wait`
  holds it while it scans the readers, which includes attaching channels,
  so `add` and `remove` can block for that long. `read`, `peek` and
  `publish` take no lock and are unaffected.
- 127 readers per set. More readers need more sets, each with its own
  waiting thread.

## Usage (non-normative)

The complete programs, in C, Python and C#, are in
[`examples/`](../examples/README.md).

```c
struct motor_status {
    uint64_t sequence;
    float    speed_rpm;
    float    current_a;
    float    temperature_c;
};
#define MOTOR_STATUS_V1 0x00010001u   /* schema 1, version 1 */

/* writer */
psmsgr_state_options o;
psmsgr_state_options_init(&o);
o.capacity     = sizeof(struct motor_status);
o.payload_type = MOTOR_STATUS_V1;

psmsgr_state_writer *w;
if (psmsgr_state_writer_open("motor", &o, &w) != PSMSGR_OK) { /* ... */ }
struct motor_status s = current_status();
psmsgr_state_publish(w, &s, sizeof s, NULL);

/* reader: consume only changes, detect a stale producer */
psmsgr_state_reader *r;
psmsgr_state_reader_open("motor", NULL, &r);
uint32_t seen = 0;
for (;;) {
    psmsgr_state_info i;
    if (psmsgr_state_peek(r, &i) == PSMSGR_OK) {
        if (psmsgr_now_ns() - i.timestamp_ns > 100000000ull) { /* >100 ms old */ }
        if (i.generation != seen &&
            psmsgr_state_read(r, &s, sizeof s, &i) == PSMSGR_OK)
            seen = i.generation;               /* use s */
    }
    /* sleep, or: psmsgr_state_wait(r, seen, 500); */
}
```

## Tools

`psmsgr-dump <name> [--dir D] [--watch] [--hex]` prints a channel's header,
its slots (seq, generation, age, length), writer liveness and, optionally,
a hexdump of the latest payload. It is built on the public API plus a
read-only raw header view, and it is shipped in `psmsgr-tools` for
debugging on the target.

- **Raw view.** The data file is opened `O_RDONLY | O_NOFOLLOW` and read
  with `pread`, never written. The header passes the attach checks of
  state-channel.md §6.1 before any other field is used, so a corrupt file
  is reported, never crashed on. The raw fields are a snapshot and may be
  torn while a writer is active.
- **`latest`** is shown decoded (slot index, tag). A tag that doesn't match
  the named slot's current `seq`, or an odd `seq` there, is flagged as
  stale: a reader gets `BUSY` until the writer publishes again. An odd
  `seq` in any slot means a write in progress, or one left by an abort or
  a crash. A `latest` that readers get `FORMAT` from (a slot index out of
  range, or a published slot whose length exceeds the capacity) is flagged
  as invalid, and the exit status is 1.
- **Consistent values** come only through the API: the latest value's
  generation, length and age (`peek`), writer liveness (`writer_alive`), and
  the `--hex` payload (`read`, at most the first 1 KiB shown).
- **`--watch`** redraws whenever the value, the writer's liveness or the file
  changes: it blocks in `psmsgr_state_wait`, polls on `NO_NOTIFY` channels,
  and exits 0 on `SIGINT` or `SIGTERM`. A missing or invalid channel is
  shown and watched, not an error.
- **Exit status:** 0 ok, 1 channel missing or invalid, 2 usage error
  (including an invalid name).
