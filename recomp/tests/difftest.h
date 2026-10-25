/* Shared layout for the differential instruction tests. */
#pragma once
#include <stdint.h>

#define DT_REGION       0x10000000u        /* data + stack, compared after each test */
#define DT_REGION_SIZE  0x00200000u
#define DT_DATA_SIZE    0x00080000u        /* first 512 KB is compared */
#define DT_STACK        0x10070000u
#define DT_FLAGS_OUT    0x1007F000u        /* test code stores eflags here (pushfd; pop [..]) */
#define DT_CODE         0x10100000u
#define DT_MAX_CHANGES  48

typedef struct {
    uint8_t code[48];
    uint32_t regs[8];
    uint32_t eflags;
    uint32_t seed;
} DtTest;

typedef struct {
    uint32_t regs[8];
    uint32_t eflags;
    uint32_t nchanges;
    uint32_t change_addr[DT_MAX_CHANGES];
    uint8_t change_val[DT_MAX_CHANGES];
} DtResult;

static inline void dt_pattern(uint8_t *p, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < DT_DATA_SIZE; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[i] = (uint8_t)x; }
}
