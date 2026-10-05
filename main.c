#include <stdint.h>
#include <stdio.h>

#include <pspkernel.h>
#include <pspme.h>

#include "common.h"

PSP_MODULE_INFO("PSP Dual-Core Actions", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

extern void me_trampoline(void);

static DualCoreBridge bridge __attribute__((aligned(64)));

int main(void)
{
    const uintptr_t trampoline_physical = (uintptr_t)me_trampoline & 0x0fffffffu;
    volatile uint32_t *uncached_state = (volatile uint32_t *)((uintptr_t)&bridge | 0x40000000u);
    volatile uint32_t *uncached_output = (volatile uint32_t *)((uintptr_t)&bridge + 4u | 0x40000000u);
    int result;
    unsigned int polls;

    bridge.input = 37u;
    bridge.output = 0u;
    bridge.state = BRIDGE_STATE_IDLE;
    sceKernelDcacheWritebackAll();

    result = sceMeBootStart(0, (void *)trampoline_physical);
    if (result < 0) {
        printf("sceMeBootStart failed: 0x%08X\\n", result);
        sceKernelExitGame();
        return 1;
    }

    for (polls = 0; polls < 1000000u; ++polls) {
        if (*uncached_state == BRIDGE_STATE_DONE) {
            printf("ME result: %u\\n", (unsigned int)*uncached_output);
            sceKernelExitGame();
            return 0;
        }
    }

    printf("ME timeout\\n");
    sceKernelExitGame();
    return 1;
}
