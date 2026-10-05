#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

typedef struct __attribute__((aligned(64))) DualCoreBridge {
    volatile uint32_t input;
    volatile uint32_t output;
    volatile uint32_t state;
    volatile uint32_t reserved[13];
} DualCoreBridge;

_Static_assert(sizeof(DualCoreBridge) == 64, "DualCoreBridge must be 64 bytes");

#define BRIDGE_STATE_IDLE 0u
#define BRIDGE_STATE_DONE 1u

#endif
