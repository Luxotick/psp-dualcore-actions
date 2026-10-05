#include <stdint.h>

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>

#include <me-safe-task/me-stask.h>

#include "common.h"

PSP_MODULE_INFO("PSP Dual-Core Actions", 0, 1, 1);
PSP_HEAP_SIZE_KB(-1024);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);

extern void me_loop(void *param);

static DualCoreBridge bridge __attribute__((aligned(64)));

static void wait_for_home(void)
{
    SceCtrlData pad;

    for (;;) {
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_HOME) {
            return;
        }
        sceDisplayWaitVblankStart();
    }
}

int main(void)
{
    Task task;
    int result;

    pspDebugScreenInit();
    pspDebugScreenPrintf("PSP ME safe-task test\n");
    pspDebugScreenPrintf("main entered; waiting before ME init...\n");
    sceDisplayWaitVblankStart();
    sceKernelDelayThread(3000000);
    pspDebugScreenPrintf("initializing dispatcher...\n");

    bridge.input = 37u;
    bridge.output = 0u;
    bridge.state = BRIDGE_STATE_IDLE;

    result = meSafeTaskInitDispatcher();
    if (result < 0) {
        pspDebugScreenPrintf("meSafeTask init failed: %d\n", result);
        wait_for_home();
        sceKernelExitGame();
        return 1;
    }

    pspDebugScreenPrintf("dispatcher ready\n");
    meSafeTaskLoadModule();
    pspDebugScreenPrintf("task module ready\n");
    sceKernelDcacheWritebackInvalidateRange(&bridge, sizeof bridge);

    task.func = me_loop;
    task.param = &bridge;
    task.index = 0;
    pspDebugScreenPrintf("dispatching task...\n");
    meSafeTaskDispatch(&task);
    meSafeTaskWaitReady();
    pspDebugScreenPrintf("task completed\n");
    sceKernelDcacheInvalidateRange(&bridge, sizeof bridge);

    pspDebugScreenPrintf("ME state: %u\n", (unsigned int)bridge.state);
    pspDebugScreenPrintf("ME result: %u\n", (unsigned int)bridge.output);
    pspDebugScreenPrintf("Press HOME to exit.\n");
    wait_for_home();
    meSafeTaskUnloadModule();
    sceKernelExitGame();
    return bridge.state == BRIDGE_STATE_DONE && bridge.output == 42u ? 0 : 1;
}
