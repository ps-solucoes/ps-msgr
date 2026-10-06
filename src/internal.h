/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Library-internal declarations. Everything here has hidden visibility, so
 * none of it is exported from libpsmsgr.so; tests that need it link the
 * static library.
 */
#ifndef PSMSGR_INTERNAL_H
#define PSMSGR_INTERNAL_H

#include <psmsgr/state.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>

#define WAIT_SLICE_NS      UINT64_C(1000000000) /* orphan check at least once per second */
#define UNATTACHED_POLL_NS UINT64_C(10000000)   /* unattached wait: retry attach every 10 ms */
#define NS_PER_MS          UINT64_C(1000000)

/* 32-bit arches have a separate futex syscall taking the 64-bit
 * struct __kernel_timespec; 64-bit arches only have the one. */
#ifdef SYS_futex_time64
#define PSMI_SYS_FUTEX SYS_futex_time64
#else
#define PSMI_SYS_FUTEX SYS_futex
#endif

static inline uint64_t psmi_clock_ns(clockid_t clock)
{
    struct timespec ts;
    /* Cannot fail for the clocks used here with a valid pointer. */
    (void)clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* memcpy for the racy side of a seqlock (state-channel.md §5.4). Defined in
 * its own translation unit so that, without LTO, the compiler can neither
 * inline it nor move its accesses across the fences around the call site.
 * Needs no TSan annotation: each handle maps the file separately, so TSan
 * never sees the writer's and a reader's copies as the same memory. */
void psmi_seq_copy(void *dst, const void *src, size_t n);

/* ---- reader wait, shared with the waitset --------------------------------- */

/* Positive, unlike the PSMSGR_E_* codes. */
enum {
    PSMI_STEP_ARMED = 1,      /* unchanged: sleep on *addr while it holds *expected */
    PSMI_STEP_UNATTACHED = 2, /* no file to sleep on: retry within UNATTACHED_POLL_NS */
};

/* One non-blocking pass of the wait loop (state-channel.md §6.6): attaches or
 * reattaches as needed, then checks the generation. Returns PSMSGR_OK
 * (*generation differs from last_generation), PSMI_STEP_ARMED,
 * PSMI_STEP_UNATTACHED, or NOTSUP | FORMAT | SYS. */
int psmi_reader_wait_step(psmsgr_state_reader *r, uint32_t last_generation, uint32_t *generation,
                          const uint32_t **addr, uint32_t *expected);

/* §6.2 orphan check: detaches if the path no longer names the mapped file.
 * OK | SYS. */
int psmi_reader_identity_check(psmsgr_state_reader *r);

/* ---- test hooks ---------------------------------------------------------- */

/* Fault injection for the lock identity check (state-channel.md §4): called
 * between opening the lock file and locking it. NULL outside of tests. */
extern void (*psmi_test_lock_opened)(void);

/* Breaks the seqlock on purpose: a reader accepts a copy even when the slot's
 * seq changed during it (§6.3). The torture test sets it to prove it detects
 * torn reads. Read only on the retry path, so it costs nothing otherwise. */
extern bool psmi_test_skip_seq_recheck;

/* Makes futex_waitv fail with ENOSYS, as on a kernel older than 5.16. */
extern bool psmi_test_waitv_enosys;

/* Sets the generation the writer's next publish will carry (must not be 0). */
void psmi_writer_set_generation(psmsgr_state_writer *w, uint32_t next);

#endif /* PSMSGR_INTERNAL_H */
