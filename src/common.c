/* SPDX-License-Identifier: Apache-2.0 */
#include <psmsgr/psmsgr.h>

#include "internal.h"

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "ps-msgr channel files are little-endian; big-endian hosts are unsupported");

uint32_t psmsgr_version(void)
{
    return PSMSGR_VERSION_NUMBER(PSMSGR_VERSION_MAJOR, PSMSGR_VERSION_MINOR, PSMSGR_VERSION_PATCH);
}

const char *psmsgr_strerror(int code)
{
    switch (code) {
    case PSMSGR_OK:              return "success";
    case PSMSGR_E_INVAL:         return "invalid argument";
    case PSMSGR_E_SYS:           return "system call failed";
    case PSMSGR_E_NODATA:        return "no data";
    case PSMSGR_E_TOOSMALL:      return "buffer too small";
    case PSMSGR_E_TOOBIG:        return "payload larger than capacity, or waitset full";
    case PSMSGR_E_BUSY:          return "channel busy";
    case PSMSGR_E_TIMEOUT:       return "timed out";
    case PSMSGR_E_INTR:          return "interrupted";
    case PSMSGR_E_WRITER_EXISTS: return "another writer holds the channel";
    case PSMSGR_E_MISMATCH:      return "channel geometry mismatch";
    case PSMSGR_E_FORMAT:        return "invalid channel format";
    case PSMSGR_E_NOTSUP:        return "not supported";
    case PSMSGR_E_STATE:         return "invalid state for this call";
    default:                     return "unknown error";
    }
}

uint64_t psmsgr_now_ns(void)
{
    return psmi_clock_ns(CLOCK_MONOTONIC);
}
