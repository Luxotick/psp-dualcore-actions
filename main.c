#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pspaudiocodec.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility_avmodules.h>
#include <me-safe-task/me-stask.h>
#include <me-safe-task/me-stask-kcall.h>
#include "common.h"

PSP_MODULE_INFO("PSP ME Two Operands", 0, 2, 0);
PSP_HEAP_SIZE_KB(-1024);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
#define BUILD_VERSION "2.0 / 2026-10-08"
#define TIMEOUT_US 2000000u
#define FAIL_TARGET -1001
#define FAIL_MEMORY -1002
#define FAIL_PROTOCOL -1003
#define FAIL_RESULT -1004
#define FAIL_MARKER -1005
#define FAIL_SEQUENCE -1006
#define FAIL_OPERANDS -1007
#define FAIL_PATH -1008

// Static application system RAM survives every asynchronous job.
static SharedTask shared_task;
static SharedAudioTask audio_task __attribute__((aligned(64)));
static int16_t audio_output[2048] __attribute__((aligned(64)));
static unsigned long codec_data[64] __attribute__((aligned(64)));
static uint32_t sequence;
static int dispatcher_ready, av_loaded, unsafe_to_exit, power_locked;
static int dispatcher_init_result, av_load_result, edram_get_result, edram_release_result;
static volatile int exit_requested;
typedef struct DeviceInfo {
    int model;
    uint32_t firmware;
    int table;
    uint32_t witness;
} DeviceInfo;
static DeviceInfo device;

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
static int has_mp3_suffix(const char *name)
{
    const size_t length = strlen(name);
    if (length < 4) return 0;
    const char *suffix = name + length - 4;
    return (suffix[0] == '.' &&
        (suffix[1] == 'm' || suffix[1] == 'M') &&
        (suffix[2] == 'p' || suffix[2] == 'P') &&
        (suffix[3] == '3'));
}
static int find_arktik_track(char *path, size_t path_size)
{
    static const char *directories[] = {
        "ms0:/MUSIC/ARKTIK",
        "ms0:/PSP/MUSIC/ARKTIK",
        "ms0:/ARKTIK"
    };
    for (unsigned int directory_index = 0;
         directory_index < sizeof directories / sizeof directories[0];
         ++directory_index) {
        const int directory = sceIoDopen(directories[directory_index]);
        if (directory < 0) continue;
        SceIoDirent entry;
        memset(&entry, 0, sizeof entry);
        while (sceIoDread(directory, &entry) > 0) {
            if ((entry.d_stat.st_mode & FIO_S_IFDIR) != 0 ||
                !has_mp3_suffix(entry.d_name)) {
                memset(&entry, 0, sizeof entry);
                continue;
            }
            const int written = snprintf(path, path_size, "%s/%s",
                directories[directory_index], entry.d_name);
            sceIoDclose(directory);
            return written > 0 && (size_t)written < path_size ? 0 : FAIL_PATH;
        }
        sceIoDclose(directory);
    }
    return FAIL_PATH;
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
static void stage(unsigned int number, const char *message)
{
    pspDebugScreenPrintf("[%02u] %s\n", number, message);
    // Expose the last stage on the LCD before a hazardous operation.
    sceDisplayWaitVblankStart();
}
static void report_return(const char *operation, int ret)
{
    pspDebugScreenPrintf("%s: 0x%08X (%d)\n", operation, (unsigned int)ret, ret);
}
static void dump_task(void)
{
    pspDebugScreenPrintf("state=%u magic=%08X ver=%u error=%u\n",
        (unsigned int)shared_task.state, (unsigned int)shared_task.magic,
        (unsigned int)shared_task.version, (unsigned int)shared_task.error);
    pspDebugScreenPrintf("A=%d B=%d ME result=%d\n", (int)shared_task.a,
        (int)shared_task.b, (int)shared_task.result);
    pspDebugScreenPrintf("ME marker=%08X seq=%u returned=%u\n",
        (unsigned int)shared_task.me_marker, (unsigned int)shared_task.sequence,
        (unsigned int)shared_task.completed_sequence);
    pspDebugScreenPrintf("ME read A=%d B=%d\n", (int)shared_task.read_a,
        (int)shared_task.read_b);
}
static int run_vector(int32_t a, int32_t b, int32_t expected)
{
    stage(5, "Preparing shared operands");
    memset(&shared_task, 0, sizeof shared_task);
    shared_task.magic = TASK_MAGIC;
    shared_task.version = TASK_VERSION;
    shared_task.a = a;
    shared_task.b = b;
    shared_task.result = RESULT_SENTINEL;
    if (++sequence == 0) ++sequence;
    shared_task.sequence = sequence;
    shared_task.state = TASK_READY;
    pspDebugScreenPrintf("CPU input A: %d  B: %d\n", (int)a, (int)b);
    stage(6, "Publishing CPU input cache");
    shared_sync();
    // Publish the input line and relinquish CPU cache ownership to ME.
    sceKernelDcacheWritebackInvalidateRange(&shared_task, sizeof shared_task);
    Task task = { me_loop, &shared_task, 0 };
    stage(7, "Dispatching to Media Engine");
    int ret = meSafeTaskDispatch(&task);
    report_return("dispatch", ret);
    if (ret < 0) {
        unsafe_to_exit = 1;
        return ret;
    }
    stage(8, "Waiting for ME (2 second limit)");
    ret = meSafeTaskWaitReadyTimeout(TIMEOUT_US);
    // Read fresh system RAM, including RUNNING/sentinels on timeout.
    sceKernelDcacheInvalidateRange(&shared_task, sizeof shared_task);
    shared_sync();
    if (ret < 0) {
        unsafe_to_exit = 1; // Never reuse storage or unload live ME callbacks.
        report_return(ret == -110 ? "ME TIMEOUT" : "ME wait", ret);
        dump_task();
        return ret;
    }
    stage(9, "ME dispatcher returned");
    stage(10, "Validating shared result");
    dump_task();
    if (shared_task.magic != TASK_MAGIC || shared_task.version != TASK_VERSION ||
        shared_task.state != TASK_DONE || shared_task.error != 0) return FAIL_PROTOCOL;
    if (shared_task.sequence != sequence ||
        shared_task.completed_sequence != sequence) return FAIL_SEQUENCE;
    if (shared_task.a != a || shared_task.b != b ||
        shared_task.read_a != a || shared_task.read_b != b) return FAIL_OPERANDS;
    if (shared_task.me_marker != ME_MARKER) return FAIL_MARKER;
    if (shared_task.result == RESULT_SENTINEL ||
        shared_task.result != expected) return FAIL_RESULT;
    stage(11, "PASS - calculation executed by ME");
    return 0;
}
static int run_audio_probe(void)
{
    static const int16_t input[] = {-32768, -16000, -1, 0, 1, 16000, 30000, 32767};
    static const int16_t expected[] = {-16384, -8000, 0, 0, 1, 8000, 15000, 16384};
    memset(&audio_task, 0, sizeof audio_task);
    audio_task.magic = TASK_MAGIC;
    audio_task.version = TASK_VERSION;
    audio_task.sample_count = sizeof input / sizeof input[0];
    audio_task.gain_q15 = 16384;
    memcpy((void *)audio_task.samples, input, sizeof input);
    if (++sequence == 0) ++sequence;
    audio_task.sequence = sequence;
    audio_task.state = AUDIO_TASK_READY;
    shared_sync();
    sceKernelDcacheWritebackInvalidateRange(&audio_task, sizeof audio_task);
    Task task = { me_audio_loop, &audio_task, 0 };
    int ret = meSafeTaskDispatch(&task);
    if (ret < 0) return ret;
    ret = meSafeTaskWaitReadyTimeout(TIMEOUT_US);
    sceKernelDcacheInvalidateRange(&audio_task, sizeof audio_task);
    shared_sync();
    if (ret < 0 || audio_task.state != AUDIO_TASK_DONE ||
        audio_task.completed_sequence != audio_task.sequence ||
        audio_task.peak != 16384) return FAIL_PROTOCOL;
    for (uint32_t i = 0; i < audio_task.sample_count; ++i)
        if (audio_task.samples[i] != expected[i]) return FAIL_RESULT;
    pspDebugScreenPrintf("PCM ME PASS: %u samples, gain=0.5, peak=%d\n",
        (unsigned int)audio_task.sample_count, (int)audio_task.peak);
    return 0;
}
static int run_audio_output_probe(void)
{
    const unsigned int block_count = 88u;
    int ret = sceAudioSRCChReserve(1024, 44100, 2);
    report_return("audio SRC reserve", ret);
    if (ret < 0) return ret;
    for (unsigned int block = 0; block < block_count; ++block) {
        audio_task.magic = TASK_MAGIC;
        audio_task.version = TASK_VERSION;
        audio_task.sample_count = AUDIO_MAX_SAMPLES;
        audio_task.gain_q15 = 24576;
        for (unsigned int frame = 0; frame < AUDIO_MAX_SAMPLES; ++frame) {
            const unsigned int phase = (block * AUDIO_MAX_SAMPLES + frame) % 100u;
            audio_task.samples[frame] = phase < 50u ? 12000 : -12000;
        }
        if (++sequence == 0) ++sequence;
        audio_task.sequence = sequence;
        audio_task.state = AUDIO_TASK_READY;
        shared_sync();
        sceKernelDcacheWritebackInvalidateRange(&audio_task, sizeof audio_task);
        Task task = { me_audio_loop, &audio_task, 0 };
        ret = meSafeTaskDispatch(&task);
        if (ret < 0) break;
        ret = meSafeTaskWaitReadyTimeout(TIMEOUT_US);
        sceKernelDcacheInvalidateRange(&audio_task, sizeof audio_task);
        shared_sync();
        if (ret < 0 || audio_task.state != AUDIO_TASK_DONE ||
            audio_task.completed_sequence != audio_task.sequence) {
            if (ret >= 0) ret = FAIL_PROTOCOL;
            break;
        }
        for (unsigned int frame = 0; frame < AUDIO_MAX_SAMPLES; ++frame) {
            const int16_t sample = audio_task.samples[frame];
            audio_output[frame * 2u] = sample;
            audio_output[frame * 2u + 1u] = sample;
        }
        ret = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, audio_output);
        if (ret < 0) break;
    }
    report_return("audio SRC output", ret);
    sceKernelDelayThread(30000);
    const int release_ret = sceAudioSRCChRelease();
    report_return("audio SRC release", release_ret);
    if (ret < 0 || release_ret < 0) {
        report_return("audio SRC probe", ret < 0 ? ret : release_ret);
        return ret < 0 ? ret : release_ret;
    }
    pspDebugScreenPrintf("AUDIO OUT PASS: 1024 stereo frames at 44.1 kHz\n");
    return 0;
}
static void title(void)
{
    pspDebugScreenClear();
    pspDebugScreenPrintf("PSP Media Engine Test  %s\n", BUILD_VERSION);
    pspDebugScreenPrintf("Model ID=%d (PSP-3000 family) FW=%08X\n",
        device.model, (unsigned int)device.firmware);
    pspDebugScreenPrintf("ME table=%d witness=%08X / Classic\n",
        device.table, (unsigned int)device.witness);
    pspDebugScreenPrintf("Init=%08X AV=%08X cache=%08X/%08X\n\n",
        (unsigned int)dispatcher_init_result, (unsigned int)av_load_result,
        (unsigned int)edram_get_result, (unsigned int)edram_release_result);
}
static void run_tests(int multiple)
{
    static const int32_t vectors[][3] = {
        {37, 7, 44}, {1234, 5678, 6912}, {100, 23, 123}, {-40, 82, 42}
    };
    int32_t returned_results[4] = {0};
    int ret = 0;
    const unsigned int count = multiple ? 4u : 2u;
    for (unsigned int i = 0; i < count; ++i) {
        title();
        pspDebugScreenPrintf("Vector %u/%u (session job %u)\n", i + 1, count,
            (unsigned int)(sequence + 1));
        ret = run_vector(vectors[i][0], vectors[i][1], vectors[i][2]);
        if (ret < 0) break;
        returned_results[i] = shared_task.result;
        if (i + 1 < count) sceKernelDelayThread(1500000);
    }
    if (ret < 0) report_return("FAIL", ret);
    else {
        pspDebugScreenPrintf("\nPASS: all %u vectors validated.\n", count);
        for (unsigned int i = 0; i < count; ++i)
            pspDebugScreenPrintf("%d + %d = %d [ME PASS]\n", (int)vectors[i][0],
                (int)vectors[i][1], (int)returned_results[i]);
    }
}
static int initialize(void)
{
    pspDebugScreenPrintf("PSP Media Engine Test  %s\n", BUILD_VERSION);
    stage(1, "Application started");
    stage(2, "Load kernel bridge / detect model");
    // Publish kernel callback code/data before calling through a kernel alias.
    sceKernelDcacheWritebackAll();
    int ret = meSafeTaskGetModel();
    report_return("kernel model query", ret);
    if (ret < 0) return ret;
    device.model = ret;
    ret = meSafeTaskCall(inspect_device, &device);
    report_return("kernel bridge", ret);
    if (ret < 0) return ret;
    pspDebugScreenPrintf("model=%d FW=%08X table=%d witness=%08X\n", device.model,
        (unsigned int)device.firmware, device.table, (unsigned int)device.witness);
    // Model IDs = generation minus one: 03g/04g/07g/09g are PSP-3000 variants.
    if ((device.model != 2 && device.model != 3 && device.model != 6 && device.model != 8) ||
        device.firmware != 0x06060110u || device.table != ME_CORE_T2_IMG_TABLE)
        return FAIL_TARGET;
    const uintptr_t data_addr = (uintptr_t)&shared_task;
    const uintptr_t code_addr = (uintptr_t)me_loop;
    // Common lower user system RAM, also used by upstream samples.
    if ((data_addr & 63u) != 0 || data_addr < 0x08800000u ||
        data_addr + sizeof shared_task > 0x0A000000u ||
        code_addr < 0x08800000u || code_addr >= 0x0A000000u) return FAIL_MEMORY;
    pspDebugScreenPrintf("shared=%08X ME func=%08X\n", (unsigned int)data_addr,
        (unsigned int)code_addr);
    ret = scePowerLock(0); // Keep suspend outside the active patch session.
    report_return("power lock", ret);
    if (ret < 0) return ret;
    power_locked = 1;
    stage(3, "Initialize ME Classic dispatcher");
    ret = meSafeTaskInitDispatcher();
    dispatcher_init_result = ret;
    report_return("ME init", ret);
    if (ret < 0) return ret;
    dispatcher_ready = 1;
    unsafe_to_exit = 1; // Until the cache activation syscall completes.
    stage(31, "Load AVCODEC module");
    ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    av_load_result = ret;
    report_return("AVCODEC load", ret);
    if (ret < 0) return ret;
    av_loaded = 1;
    stage(32, "Publish code / activate ME I-cache hook");
    // Once at init: publish relocated code, selected mapping and patch data
    // before ME invalidates I-cache through the getEDRAM hook.
    sceKernelDcacheWritebackAll();
    ret = sceAudiocodecGetEDRAM(codec_data, 0x1000);
    edram_get_result = ret;
    report_return("getEDRAM", ret);
    if (ret < 0) return ret;
    stage(33, "Release temporary codec EDRAM");
    ret = sceAudiocodecReleaseEDRAM(codec_data);
    edram_release_result = ret;
    report_return("releaseEDRAM", ret);
    if (ret < 0) return ret;
    unsafe_to_exit = 0;
    stage(4, "ME initialization OK");
    sceKernelDelayThread(1500000);
    return 0;
}
int main(int argc, char **argv)
{
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    stage(0, "Set game directory / exit callback");
    int init_result = set_application_directory(argc, argv);
    report_return("game directory", init_result);
    if (init_result >= 0) {
        init_result = sceKernelCreateCallback("ME proof exit", exit_callback, NULL);
        report_return("exit callback create", init_result);
        if (init_result >= 0) {
            init_result = sceKernelRegisterExitCallback(init_result);
            report_return("exit callback register", init_result);
        }
    }
    if (init_result >= 0) init_result = initialize();
    if (init_result < 0) report_return("INIT FAIL", init_result);
    else {
        run_tests(0);
        if (run_audio_probe() < 0)
            pspDebugScreenPrintf("PCM ME probe FAILED\n");
        else if (run_audio_output_probe() < 0)
            pspDebugScreenPrintf("AUDIO OUT probe FAILED\n");
        char arktik_track[512];
        const int track_result = find_arktik_track(arktik_track, sizeof arktik_track);
        report_return("ARKTIK scan", track_result);
        if (track_result == 0)
            pspDebugScreenPrintf("ARKTIK track: %s\n", arktik_track);
    }
    uint32_t previous_buttons = 0;
controls:
    for (;;) {
        pspDebugScreenSetXY(0, 30);
        if (unsafe_to_exit)
            pspDebugScreenPrintf("START=reboot PSP (ME state uncertain)       ");
        else if (init_result < 0)
            pspDebugScreenPrintf("START=exit                                 ");
        else
            pspDebugScreenPrintf("X=repeat  TRIANGLE=4 vectors  START=exit     ");
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        const uint32_t pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;
        if ((pressed & PSP_CTRL_START) || (exit_requested && !unsafe_to_exit)) {
            if (unsafe_to_exit) {
                pspDebugScreenSetXY(0, 25);
                stage(90, "Cold reboot requested");
                const int ret = meSafeTaskCall(reboot_device, NULL);
                report_return("reboot", ret);
            } else break;
        }
        if (init_result == 0 && !unsafe_to_exit &&
            (pressed & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)))
            run_tests((pressed & PSP_CTRL_TRIANGLE) != 0);
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThreadCB(20000);
    }
    if (dispatcher_ready) {
        pspDebugScreenSetXY(0, 25);
        stage(12, "Restore ME firmware instructions");
        const int ret = meSafeTaskShutdownDispatcher(TIMEOUT_US);
        report_return("ME shutdown", ret);
        if (ret < 0) {
            pspDebugScreenPrintf("Cleanup failed. START requests cold reboot.\n");
            unsafe_to_exit = 1;
            exit_requested = 0;
            goto controls; // Keep storage resident; no unbounded completion wait.
        }
    }
    if (av_loaded) report_return("AVCODEC unload",
        sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC));
    if (power_locked) scePowerUnlock(0);
    sceKernelExitGame();
    return init_result < 0 ? 1 : 0;
}
