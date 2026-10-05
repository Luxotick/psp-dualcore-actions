#include <stdint.h>

#include <pspdebug.h>
#include <pspkernel.h>
#include <psppower.h>

#include <me-safe-task/me-stask.h>

#include "common.h"

PSP_MODULE_INFO("PSP Dual-Core Actions", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

extern void me_loop(void *param);

static DualCoreBridge bridge __attribute__((aligned(64)));

int main(void)
{
    Task task;
    int result;

    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenInit();

    bridge.input = 37u;
    bridge.output = 0u;
    bridge.state = BRIDGE_STATE_IDLE;

    result = meSafeTaskInitDispatcher();
    if (result < 0) {
        pspDebugScreenPrintf("meSafeTask init failed: %d\\n", result);
        sceKernelExitGame();
        return 1;
    }

    meSafeTaskLoadModule();
    sceKernelDcacheWritebackInvalidateRange(&bridge, sizeof bridge);

    task.func = me_loop;
    task.param = &bridge;
    task.index = 0;
    meSafeTaskDispatch(&task);
    meSafeTaskWaitReady();
    sceKernelDcacheInvalidateRange(&bridge, sizeof bridge);

    pspDebugScreenPrintf("ME state: %u\\n", (unsigned int)bridge.state);
    pspDebugScreenPrintf("ME result: %u\\n", (unsigned int)bridge.output);
    sceKernelDelayThread(1000000);
    meSafeTaskUnloadModule();
    sceKernelExitGame();
    return bridge.state == BRIDGE_STATE_DONE && bridge.output == 42u ? 0 : 1;
}
