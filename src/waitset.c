/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Waitsets: one caller thread waits on many readers with futex_waitv. The
 * protocol is spec/state-channel.md §6.7; the interface is spec/c-api.md.
 */
#include <psmsgr/waitset.h>

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/time_types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The same number on every architecture; older libc headers lack it. */
#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif

#define KICK_FLAGS (FUTEX_32 | FUTEX_PRIVATE_FLAG)

bool psmi_test_waitv_enosys;

typedef struct {
    psmsgr_state_reader *r;
    uint64_t token;
    uint64_t retry_ns; /* unattached: next attach attempt; 0 when attached */
    uint32_t last_generation;
} ws_entry;

struct psmsgr_waitset {
    /* Atomic. */
    uint32_t lock;         /* 0 free, 1 held, 2 held with waiters */
    uint32_t kick;         /* private futex in every futex_waitv: bumped to end one */
    uint32_t wake_pending; /* a wake not yet seen by wait */
    uint32_t exits;        /* bumped when wait leaves futex_waitv */
    /* Under lock. */
    bool waiting;   /* a thread is in wait */
    bool in_kernel; /* it is in futex_waitv, or about to enter it */
    bool remover;   /* a remove waits for `exits` */
    uint32_t count;
    uint64_t check_ns; /* next orphan identity check */
    ws_entry entry[PSMSGR_WAITSET_MAX];
    /* The waiting thread's. */
    struct futex_waitv wv[PSMSGR_WAITSET_MAX + 1];
};

/* ---- private futexes ------------------------------------------------------ */

static void pfutex_wait(uint32_t *addr, uint32_t expected)
{
    (void)syscall(PSMI_SYS_FUTEX, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

static void pfutex_wake(uint32_t *addr, int count)
{
    (void)syscall(PSMI_SYS_FUTEX, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

/* A mutex that needs no libpthread (Drepper, "Futexes Are Tricky", mutex 2). */
static void ws_lock(psmsgr_waitset *ws)
{
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&ws->lock, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(&ws->lock, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        pfutex_wait(&ws->lock, 2);
        c = __atomic_exchange_n(&ws->lock, 2, __ATOMIC_ACQUIRE);
    }
}

static void ws_unlock(psmsgr_waitset *ws)
{
    if (__atomic_exchange_n(&ws->lock, 0, __ATOMIC_RELEASE) == 2)
        pfutex_wake(&ws->lock, 1);
}

/* Ends the waiting thread's futex_waitv, or makes it fail at once with EAGAIN
 * if it has not entered it yet: its expected value for `kick` is stale. */
static void ws_kick(psmsgr_waitset *ws)
{
    __atomic_fetch_add(&ws->kick, 1, __ATOMIC_SEQ_CST);
    pfutex_wake(&ws->kick, INT_MAX);
}

/* deadline_ns: absolute, on CLOCK_MONOTONIC; UINT64_MAX waits indefinitely.
 * Returns the index of the futex woken, or -1 with errno set. */
static int futex_waitv(struct futex_waitv *wv, uint32_t n, uint64_t deadline_ns)
{
    if (psmi_test_waitv_enosys) {
        errno = ENOSYS;
        return -1;
    }
    struct __kernel_timespec ts = {
        .tv_sec = (__kernel_time64_t)(deadline_ns / 1000000000u),
        .tv_nsec = (long long)(deadline_ns % 1000000000u),
    };
    return (int)syscall(SYS_futex_waitv, wv, n, 0, deadline_ns == UINT64_MAX ? NULL : &ts,
                        CLOCK_MONOTONIC);
}

static struct futex_waitv wv_entry(const uint32_t *addr, uint32_t expected, uint32_t flags)
{
    return (struct futex_waitv){ .val = expected, .uaddr = (uintptr_t)addr, .flags = flags };
}

/* ---- waitset -------------------------------------------------------------- */

int psmsgr_waitset_open(psmsgr_waitset **out)
{
    if (out == NULL)
        return PSMSGR_E_INVAL;
    *out = NULL;
    psmsgr_waitset *ws = calloc(1, sizeof *ws);
    if (ws == NULL)
        return PSMSGR_E_SYS;
    /* Probe: an expected value that doesn't match fails at once with EAGAIN
     * (or ETIMEDOUT: the deadline is in the past). */
    ws->wv[0] = wv_entry(&ws->kick, 1, KICK_FLAGS);
    if (futex_waitv(ws->wv, 1, 0) < 0 && errno != EAGAIN && errno != ETIMEDOUT) {
        int e = errno;
        free(ws);
        errno = e;
        return e == ENOSYS || e == EPERM ? PSMSGR_E_NOTSUP : PSMSGR_E_SYS;
    }
    ws->check_ns = psmi_clock_ns(CLOCK_MONOTONIC) + WAIT_SLICE_NS;
    *out = ws;
    return PSMSGR_OK;
}

void psmsgr_waitset_close(psmsgr_waitset *ws)
{
    free(ws);
}

static uint32_t ws_find(const psmsgr_waitset *ws, const psmsgr_state_reader *r)
{
    uint32_t i = 0;
    while (i < ws->count && ws->entry[i].r != r)
        ++i;
    return i;
}

int psmsgr_waitset_add(psmsgr_waitset *ws, psmsgr_state_reader *r, uint32_t last_generation,
                       uint64_t token)
{
    if (ws == NULL || r == NULL)
        return PSMSGR_E_INVAL;
    int rc = PSMSGR_OK;
    ws_lock(ws);
    if (ws_find(ws, r) < ws->count) {
        rc = PSMSGR_E_STATE;
    } else if (ws->count == PSMSGR_WAITSET_MAX) {
        rc = PSMSGR_E_TOOBIG;
    } else {
        ws->entry[ws->count++] =
            (ws_entry){ .r = r, .token = token, .last_generation = last_generation };
        if (ws->in_kernel)
            ws_kick(ws);
    }
    ws_unlock(ws);
    return rc;
}

int psmsgr_waitset_remove(psmsgr_waitset *ws, psmsgr_state_reader *r)
{
    if (ws == NULL || r == NULL)
        return PSMSGR_E_INVAL;
    ws_lock(ws);
    uint32_t i = ws_find(ws, r);
    if (i == ws->count) {
        ws_unlock(ws);
        return PSMSGR_E_NODATA;
    }
    memmove(&ws->entry[i], &ws->entry[i + 1], (ws->count - i - 1) * sizeof ws->entry[0]);
    ws->count--;
    /* The kernel may still hold an address in r's mapping: once the caller
     * closes r, it could be unmapped or reused. Wait until wait is out. */
    bool in_kernel = ws->in_kernel;
    uint32_t exits = __atomic_load_n(&ws->exits, __ATOMIC_RELAXED);
    if (in_kernel) {
        ws->remover = true;
        ws_kick(ws);
    }
    ws_unlock(ws);
    while (in_kernel && __atomic_load_n(&ws->exits, __ATOMIC_ACQUIRE) == exits)
        pfutex_wait(&ws->exits, exits);
    return PSMSGR_OK;
}

void psmsgr_waitset_wake(psmsgr_waitset *ws)
{
    if (ws == NULL)
        return;
    __atomic_store_n(&ws->wake_pending, 1, __ATOMIC_SEQ_CST);
    ws_kick(ws);
}

/* One pass over the entries, under lock (§6.7). Reports up to cap finished
 * entries and removes them; arms ws->wv[0..*nwv) for the others. Returns
 * when the next timer is due: an attach retry or the orphan check. */
static uint64_t scan(psmsgr_waitset *ws, uint64_t now, psmsgr_waitset_event *events, uint32_t cap,
                     uint32_t *n, uint32_t *nwv)
{
    bool check = now >= ws->check_ns, complete = true;
    uint64_t timer = UINT64_MAX;
    uint32_t kept = 0, armed = 0;
    for (uint32_t i = 0; i < ws->count; ++i) {
        ws_entry e = ws->entry[i];
        if (*n == cap || now < e.retry_ns) {
            complete &= *n < cap;
            if (e.retry_ns != 0 && e.retry_ns < timer)
                timer = e.retry_ns;
            ws->entry[kept++] = e;
            continue;
        }
        uint32_t generation = 0, expected = 0;
        const uint32_t *addr = NULL;
        int rc = check ? psmi_reader_identity_check(e.r) : PSMSGR_OK;
        if (rc == PSMSGR_OK)
            rc = psmi_reader_wait_step(e.r, e.last_generation, &generation, &addr, &expected);
        if (rc == PSMI_STEP_ARMED) {
            e.retry_ns = 0;
            ws->wv[armed++] = wv_entry(addr, expected, FUTEX_32); /* shared: the writer wakes it */
            ws->entry[kept++] = e;
        } else if (rc == PSMI_STEP_UNATTACHED) {
            e.retry_ns = now + UNATTACHED_POLL_NS;
            if (e.retry_ns < timer)
                timer = e.retry_ns;
            ws->entry[kept++] = e;
        } else {
            events[(*n)++] = (psmsgr_waitset_event){
                .token = e.token,
                .status = rc,
                .generation = generation,
                .sys_errno = rc == PSMSGR_E_SYS ? errno : 0,
            };
        }
    }
    ws->count = kept;
    *nwv = armed;
    if (check && complete)
        ws->check_ns = now + WAIT_SLICE_NS;
    if (armed > 0 && ws->check_ns < timer)
        timer = ws->check_ns;
    return timer;
}

/* Sleeps in futex_waitv on ws->wv[0..nwv) without the lock, which it drops
 * and retakes. OK (woken, EAGAIN, ETIMEDOUT: scan again) | INTR | SYS. */
static int sleep_unlocked(psmsgr_waitset *ws, uint32_t nwv, uint64_t deadline_ns)
{
    ws->in_kernel = true;
    ws_unlock(ws);
    int woken = futex_waitv(ws->wv, nwv, deadline_ns);
    int err = errno;
    ws_lock(ws);
    ws->in_kernel = false;
    __atomic_fetch_add(&ws->exits, 1, __ATOMIC_RELEASE);
    if (ws->remover) {
        ws->remover = false;
        pfutex_wake(&ws->exits, INT_MAX);
    }
    errno = err;
    if (woken >= 0 || err == EAGAIN || err == ETIMEDOUT)
        return PSMSGR_OK;
    return err == EINTR ? PSMSGR_E_INTR : PSMSGR_E_SYS;
}

int psmsgr_waitset_wait(psmsgr_waitset *ws, int32_t timeout_ms, psmsgr_waitset_event *events,
                        uint32_t cap, uint32_t *n)
{
    if (n != NULL)
        *n = 0;
    if (ws == NULL || events == NULL || cap == 0 || n == NULL)
        return PSMSGR_E_INVAL;
    uint64_t deadline = timeout_ms < 0
                            ? UINT64_MAX
                            : psmi_clock_ns(CLOCK_MONOTONIC) + (uint64_t)timeout_ms * NS_PER_MS;
    ws_lock(ws);
    if (ws->waiting) {
        ws_unlock(ws);
        return PSMSGR_E_STATE;
    }
    ws->waiting = true;
    int rc;
    do {
        /* Loaded BEFORE the scan and the wake check: a kick after this makes
         * futex_waitv return at once (§6.7). */
        uint32_t k = __atomic_load_n(&ws->kick, __ATOMIC_SEQ_CST);
        uint64_t now = psmi_clock_ns(CLOCK_MONOTONIC);
        uint32_t nwv;
        uint64_t timer = scan(ws, now, events, cap, n, &nwv);
        if (__atomic_exchange_n(&ws->wake_pending, 0, __ATOMIC_SEQ_CST) != 0 || *n > 0) {
            rc = PSMSGR_OK;
            break;
        }
        if (now >= deadline) {
            rc = PSMSGR_E_TIMEOUT;
            break;
        }
        ws->wv[nwv++] = wv_entry(&ws->kick, k, KICK_FLAGS);
        rc = sleep_unlocked(ws, nwv, timer < deadline ? timer : deadline);
    } while (rc == PSMSGR_OK);
    ws->waiting = false;
    int err = errno;
    ws_unlock(ws);
    errno = err;
    return rc;
}
