/* Pure policy: iOS thermal states are nominal=0, fair=1, serious=2, critical=3.
 * Keep producer and presentation at the same cap. Wait for 20 seconds of
 * nominal temperature before restoring 60fps to avoid rapid oscillation. */
#pragma once
#include <stdint.h>
typedef struct { uint64_t cool_until_us; } Nfs4PowerPolicy;
static inline int nfs4_power_cap(Nfs4PowerPolicy *p, int requested, int thermal,
                                int low_power, int active, uint64_t now_us) {
    if (thermal >= 1) p->cool_until_us = now_us + 20000000u;
    if (!active) return 5;
    if (thermal >= 3) return 15;
    if (requested <= 30 || low_power || thermal >= 1 || now_us < p->cool_until_us) return 30;
    return 60;
}
