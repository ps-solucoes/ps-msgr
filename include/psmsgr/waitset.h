/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Waitsets: one thread waits on many state readers at once. Added in 1.1.
 * Interface: spec/c-api.md. Protocol: spec/state-channel.md §6.7.
 */
#ifndef PSMSGR_WAITSET_H
#define PSMSGR_WAITSET_H

#include <psmsgr/psmsgr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The reader is spelled `struct psmsgr_state_reader` (the typedef is in
 * state.h): psmsgr.h includes this header, also from inside state.h. */
struct psmsgr_state_reader;

/* Readers per waitset: futex_waitv takes 128 futexes, one is the set's own. */
#define PSMSGR_WAITSET_MAX 127

/* Not thread-safe like other handles, except that add, remove and wake may be
 * called from any thread, also while another thread is in wait. The library
 * creates no thread: the caller dedicates one to wait. */
typedef struct psmsgr_waitset psmsgr_waitset;

/* A registration that finished. Fixed layout: wait fills an array of these. */
typedef struct psmsgr_waitset_event {
    uint64_t token;      /* as passed to psmsgr_waitset_add */
    int32_t status;      /* OK: the generation differs from last_generation; else what
                          * psmsgr_state_wait would return: NOTSUP | FORMAT | SYS */
    uint32_t generation; /* status OK: the generation now; else 0 */
    int32_t sys_errno;   /* status SYS: errno of the failed call; else 0 */
    uint32_t reserved;   /* 0 */
} psmsgr_waitset_event;

/* Creates an empty waitset. NOTSUP without futex_waitv: Linux < 5.16,
 * qemu-user, or a seccomp filter that returns ENOSYS or EPERM.
 * Errors: INVAL, NOTSUP, SYS (ENOMEM). */
PSMSGR_API int psmsgr_waitset_open(psmsgr_waitset **out);

/* Frees the set. Registered readers stay open and are the caller's again. No
 * other call on the set may be in progress. NULL is a no-op. */
PSMSGR_API void psmsgr_waitset_close(psmsgr_waitset *ws);

/* Registers r until its generation differs from last_generation (0 = "any
 * value"), with psmsgr_state_wait's semantics; token comes back in the event.
 * The set owns r until wait reports it or remove succeeds: until then the
 * caller must not use r in any way, including close. May block while another
 * thread's wait scans the set (attaching channels).
 * OK | INVAL | TOOBIG (PSMSGR_WAITSET_MAX registered) | STATE (r is registered). */
PSMSGR_API int psmsgr_waitset_add(psmsgr_waitset *ws, struct psmsgr_state_reader *r,
                                  uint32_t last_generation, uint64_t token);

/* Unregisters r; the caller may close it as soon as this returns, even while
 * another thread is blocked in wait. Blocks until that wait has left the
 * kernel, which it does at once.
 * OK | NODATA (not registered: never added, or already reported) | INVAL. */
PSMSGR_API int psmsgr_waitset_remove(psmsgr_waitset *ws, struct psmsgr_state_reader *r);

/* Blocks until a registered reader finishes or psmsgr_waitset_wake is called.
 * Writes up to cap events and sets *n; reported readers are unregistered and
 * the caller's again, the rest stay registered. One thread at a time.
 * timeout_ms < 0: infinite, 0: check once.
 * OK (*n >= 1, or 0 after a wake) | TIMEOUT | INTR | STATE (another thread is
 * in wait) | INVAL | SYS. *n is 0 unless OK. */
PSMSGR_API int psmsgr_waitset_wait(psmsgr_waitset *ws, int32_t timeout_ms,
                                   psmsgr_waitset_event *events, uint32_t cap, uint32_t *n);

/* Makes the current wait return, or the next one if none is in progress. Any
 * thread; never blocks. NULL is a no-op. */
PSMSGR_API void psmsgr_waitset_wake(psmsgr_waitset *ws);

#ifdef __cplusplus
}
#endif

#endif /* PSMSGR_WAITSET_H */
