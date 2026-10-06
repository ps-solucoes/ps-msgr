/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ps-msgr: inter-process state exchange. Umbrella header and common API.
 * Specification: spec/c-api.md
 */
#ifndef PSMSGR_PSMSGR_H
#define PSMSGR_PSMSGR_H

#include <stdint.h>

#if defined(__GNUC__)
#define PSMSGR_API __attribute__((visibility("default")))
#else
#define PSMSGR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Library major == SONAME number. The first release is 1.0.0; until then the
 * API/ABI may change freely. CMake reads the version from these lines. */
#define PSMSGR_VERSION_MAJOR 1
#define PSMSGR_VERSION_MINOR 1
#define PSMSGR_VERSION_PATCH 0

#define PSMSGR_VERSION_NUMBER(major, minor, patch) \
    (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))

enum {
    PSMSGR_OK = 0,
    PSMSGR_E_INVAL = -1,    /* bad argument or channel name              */
    PSMSGR_E_SYS = -2,      /* system call failed; see errno             */
    PSMSGR_E_NODATA = -3,   /* channel absent, nothing published yet, or */
                            /* reader not in the waitset                 */
    PSMSGR_E_TOOSMALL = -4, /* buffer too small; info->length is valid   */
    PSMSGR_E_TOOBIG = -5,   /* payload over capacity, or waitset full    */
    PSMSGR_E_BUSY = -6,     /* read retries exhausted; transient, retry  */
    PSMSGR_E_TIMEOUT = -7,
    PSMSGR_E_INTR = -8,          /* wait interrupted by a signal              */
    PSMSGR_E_WRITER_EXISTS = -9, /* another writer holds the channel          */
    PSMSGR_E_MISMATCH = -10,     /* existing channel has other geometry       */
    PSMSGR_E_FORMAT = -11,       /* bad magic/version/size, corrupt file      */
    PSMSGR_E_NOTSUP = -12,       /* NO_NOTIFY channel; no futex_waitv         */
    PSMSGR_E_STATE = -13,        /* call not valid now, e.g. commit w/o begin */
};

/* PSMSGR_VERSION_NUMBER() of the loaded library. */
PSMSGR_API uint32_t psmsgr_version(void);

/* Static string, never NULL. Unknown codes yield "unknown error". */
PSMSGR_API const char *psmsgr_strerror(int code);

/* CLOCK_MONOTONIC in nanoseconds: the clock used for timestamp_ns. */
PSMSGR_API uint64_t psmsgr_now_ns(void);

#ifdef __cplusplus
}
#endif

#include <psmsgr/state.h>
#include <psmsgr/waitset.h>

#endif /* PSMSGR_PSMSGR_H */
