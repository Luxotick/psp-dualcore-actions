#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspaudiocodec.h>
#include <psptypes.h>
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_avmodules.h>
#include <psputility_modules.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include "spotify/http.h"
#include "spotify/log.h"
#include "spotify/player.h"
#include "spotify/ui.h"
#include <me-safe-task/me-stask.h>
#include <me-safe-task/me-stask-kcall.h>
#include "common.h"

PSP_MODULE_INFO("PSPotify ME", 0, 2, 0);
/* Fixed heap: a negative (leave-N) size still left only ~146 KB free after
 * ME/AV/net init on hardware, too little for the HTTP/SSL net modules.
 * 24 MB holds a whole Ogg file (160 kbps: ~1.2 MB per minute) in RAM and
 * still leaves ~16 MB for firmware modules. */
PSP_HEAP_SIZE_KB(24576);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
#define TIMEOUT_US 2000000u
#define FAIL_TARGET -1001
#define FAIL_MEMORY -1002
#define FAIL_PATH -1008

// Static application system RAM survives every asynchronous job.
static SharedTask shared_task;
static unsigned long codec_data[64] __attribute__((aligned(64)));
static int dispatcher_ready, av_loaded, network_ready, unsafe_to_exit, power_locked;
static volatile int exit_requested;
typedef struct DeviceInfo {
    int model;
    uint32_t firmware;
    int table;
    uint32_t witness;
} DeviceInfo;
static DeviceInfo device;
static void report_return(const char *operation, int ret);

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    exit_requested = 1; // HOME uses the same checked teardown as START.
    return 0;
}

static int set_application_directory(int argc, char **argv)
{
    char directory[512];
    if (argc < 1 || argv == NULL || argv[0] == NULL) return FAIL_PATH;
    const char *slash = strrchr(argv[0], '/');
    if (slash == NULL) return FAIL_PATH;
    const size_t length = (size_t)(slash - argv[0]);
    if (length == 0 || length >= sizeof directory) return FAIL_PATH;
    memcpy(directory, argv[0], length);
    directory[length] = '\0';
    // The embedded bridge writes ./kcall.prx; never depend on launcher cwd.
    return sceIoChdir(directory);
}

static int network_probe(void)
{
    int ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (ret < 0 && ret != (int)0x80110801) return ret;
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (ret < 0 && ret != (int)0x80110801) return ret;
    ret = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetInetInit();
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetResolverInit();
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetApctlInit(0x1800, 0x30);
    if (ret < 0 && ret != (int)0x80410201) return ret;
    network_ready = 1;

    if (sceWlanGetSwitchState() == 0) {
        log_printf("WARNING: WLAN switch is physically OFF!\n");
    }

    int valid_profiles[16];
    int valid_count = 0;

    for (int i = 1; i <= 10; ++i) {
        if (sceUtilityCheckNetParam(i) == 0) {
            valid_profiles[valid_count++] = i;
        }
    }
    if (valid_count == 0 && sceUtilityCheckNetParam(0) == 0) {
        valid_profiles[valid_count++] = 0;
    }

    if (valid_count == 0) {
        log_printf("Wi-Fi: No profile found in PSP settings!\n");
        return -112;
    }

    int connected_profile = -1;
    for (int p = 0; p < valid_count; ++p) {
        int prof = valid_profiles[p];
        ret = sceNetApctlConnect(prof);
        if (ret == 0) {
            connected_profile = prof;
            break;
        }
    }

    if (connected_profile < 0) {
        log_printf("Wi-Fi: Connect failed!\n");
        return ret;
    }

    log_printf("Wi-Fi: Connecting (Profile %d)...", connected_profile);

    for (unsigned int attempt = 0; attempt < 300u; ++attempt) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        ret = sceNetApctlGetState(&state);
        if (ret < 0) return ret;

        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof info);
            if (sceNetApctlGetInfo(8, &info) >= 0) {
                log_printf(" OK! (IP: %s)\n", info.ip);
            } else {
                log_printf(" OK!\n");
            }
            return 0;
        }
        if (state == PSP_NET_APCTL_STATE_DISCONNECTED && attempt > 40u) {
            log_printf(" Disconnected\n");
            return -111;
        }
        sceKernelDelayThread(100000);
    }
    log_printf(" Timeout\n");
    return -110;
}

// These callbacks run on SC in kernel mode through the embedded bridge PRX.
static int inspect_device(void *param)
{
    DeviceInfo *info = param;
    info->firmware = (uint32_t)sceKernelDevkitVersion();
    info->witness = hw(ME_CORE_KERNEL_ADDR | 0x18u);
    info->table = meCoreGetTableIdFromWitnessWord();
    return 0;
}
static int reboot_device(void *unused)
{
    (void)unused;
    return scePowerRequestColdReset(0);
}
static void log_free_mem(const char *stage_name)
{
    log_printf("MEM %s: free %u KB, max block %u KB\n", stage_name,
               (unsigned int)(sceKernelTotalFreeMemSize() / 1024),
               (unsigned int)(sceKernelMaxFreeMemSize() / 1024));
}
static void report_return(const char *operation, int ret)
{
    log_printf("%s: 0x%08X (%d)\n", operation, (unsigned int)ret, ret);
}
static int initialize(void)
{
    sceKernelDcacheWritebackAll();
    int ret = meSafeTaskGetModel();
    if (ret < 0) return ret;
    device.model = ret;
    ret = meSafeTaskCall(inspect_device, &device);
    if (ret < 0) return ret;
    if ((device.model != 2 && device.model != 3 && device.model != 6 && device.model != 8) ||
        device.firmware != 0x06060110u || device.table != ME_CORE_T2_IMG_TABLE)
        return FAIL_TARGET;
    const uintptr_t data_addr = (uintptr_t)&shared_task;
    const uintptr_t code_addr = (uintptr_t)me_loop;
    if ((data_addr & 63u) != 0 || data_addr < 0x08800000u ||
        data_addr + sizeof shared_task > 0x0A000000u ||
        code_addr < 0x08800000u || code_addr >= 0x0A000000u) return FAIL_MEMORY;
    ret = scePowerLock(0);
    if (ret < 0) return ret;
    power_locked = 1;
    ret = meSafeTaskInitDispatcher();
    if (ret < 0) return ret;
    dispatcher_ready = 1;
    unsafe_to_exit = 1;
    ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    if (ret < 0) return ret;
    av_loaded = 1;
    sceKernelDcacheWritebackAll();
    ret = sceAudiocodecGetEDRAM(codec_data, 0x1000);
    if (ret < 0) return ret;
    ret = sceAudiocodecReleaseEDRAM(codec_data);
    if (ret < 0) return ret;
    unsafe_to_exit = 0;
    log_printf("PSP-3000 (model %d) ME OK\n", device.model);
    return 0;
}
int main(int argc, char **argv)
{
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    int init_result = set_application_directory(argc, argv);
    if (init_result >= 0) log_init();
    log_free_mem("start");
    if (init_result >= 0) {
        init_result = sceKernelCreateCallback("ME proof exit", exit_callback, NULL);
        if (init_result >= 0) {
            init_result = sceKernelRegisterExitCallback(init_result);
        }
    }
    if (init_result >= 0) init_result = initialize();
    int quit_now = 0;
    if (init_result < 0) {
        log_printf("INIT FAIL: 0x%08X\n", (unsigned int)init_result);
    } else if (network_probe() < 0) {
        log_printf("NETWORK FAILED\n");
    } else {
        log_free_mem("after net init");
        http_init();
        if (spotify_ui_run(&exit_requested) == PLAYER_ERR_ME_TIMEOUT) {
            /* A decode may still be running on the ME: never unload under it. */
            unsafe_to_exit = 1;
        } else {
            quit_now = 1;
        }
        pspDebugScreenInit();
        log_set_echo(1);
        if (unsafe_to_exit) log_printf("Media Engine stopped responding.\n");
    }
    uint32_t previous_buttons = 0;
controls:
    for (;;) {
        if (quit_now && !unsafe_to_exit) break;
        pspDebugScreenSetXY(0, 30);
        if (unsafe_to_exit)
            pspDebugScreenPrintf("START=reboot PSP                           ");
        else
            pspDebugScreenPrintf("START=exit                                 ");
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        const uint32_t pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;
        if ((pressed & PSP_CTRL_START) || (exit_requested && !unsafe_to_exit)) {
            if (unsafe_to_exit) {
                pspDebugScreenSetXY(0, 25);
                const int ret = meSafeTaskCall(reboot_device, NULL);
                report_return("reboot", ret);
            } else break;
        }
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThreadCB(20000);
    }
    if (dispatcher_ready) {
        pspDebugScreenSetXY(0, 25);
        const int ret = meSafeTaskShutdownDispatcher(TIMEOUT_US);
        if (ret < 0) {
            log_printf("ME shutdown failed. START=reboot.\n");
            unsafe_to_exit = 1;
            exit_requested = 0;
            quit_now = 0;
            goto controls;
        }
    }
    if (av_loaded) sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
    if (network_ready) {
        http_term();
        sceNetApctlDisconnect();
        sceNetApctlTerm();
        sceNetResolverTerm();
        sceNetInetTerm();
        sceNetTerm();
        sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }
    if (power_locked) scePowerUnlock(0);
    sceKernelExitGame();
    return init_result < 0 ? 1 : 0;
}
