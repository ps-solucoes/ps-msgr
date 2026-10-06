// SPDX-License-Identifier: Apache-2.0
// The public headers compile as C++ and declare C linkage.
#include <psmsgr/psmsgr.h>

static_assert(sizeof(psmsgr_state_info) == 24, "psmsgr_state_info size");
static_assert(sizeof(psmsgr_waitset_event) == 24, "psmsgr_waitset_event size");

int main()
{
    return psmsgr_version() == 0 ? 1 : 0;
}
