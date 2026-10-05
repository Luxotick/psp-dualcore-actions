#include <stdint.h>

#include "common.h"

void me_loop(DualCoreBridge *bridge)
{
    volatile uint32_t *uncached_input = (volatile uint32_t *)((uintptr_t)bridge | 0x40000000u);
    volatile uint32_t *uncached_output = (volatile uint32_t *)((uintptr_t)bridge + 4u | 0x40000000u);
    volatile uint32_t *uncached_state = (volatile uint32_t *)((uintptr_t)bridge + 8u | 0x40000000u);

    *uncached_output = *uncached_input + 5u;
    *uncached_state = BRIDGE_STATE_DONE;

    for (;;) {
        __asm__ volatile ("wait");
    }
}
