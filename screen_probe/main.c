#include <pspctrl.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspkernel.h>

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define BUFFER_WIDTH 512
#define FRAMEBUFFER_SIZE (BUFFER_WIDTH * SCREEN_HEIGHT * 4)

PSP_MODULE_INFO("PSP Screen Probe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static unsigned int display_list[4096] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    SceCtrlData pad;

    (void)argc;
    (void)argv;

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0x00000000, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)FRAMEBUFFER_SIZE, BUFFER_WIDTH);
    sceGuDepthBuffer((void *)(FRAMEBUFFER_SIZE * 2), BUFFER_WIDTH);
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuClearDepth(0);
    sceGuClearColor(0xff20c060);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    sceGuSwapBuffers();

    for (;;) {
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_HOME) {
            break;
        }
        sceDisplayWaitVblankStart();
    }

    sceKernelExitGame();
    return 0;
}
