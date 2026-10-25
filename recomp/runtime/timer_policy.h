/* No registered deadline means no periodic wakeup. Round upward so a future
 * timer is not polled repeatedly just before its deadline. */
#pragma once
#include <stdint.h>
#include <limits.h>
static inline uint32_t nfs4_timer_wait_ms(uint64_t now,uint64_t next) {
    if(next==UINT64_MAX)return UINT32_MAX;
    if(next<=now)return 1;
    uint64_t us=next-now;
    uint64_t ms=us/1000+(us%1000!=0);
    return ms>=UINT32_MAX?UINT32_MAX-1:(uint32_t)ms;
}
