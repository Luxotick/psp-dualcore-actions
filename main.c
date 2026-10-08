#define ENABLE_LOCAL_MP3 0
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pspaudiocodec.h>
#include <pspaudio.h>
#include <psptypes.h>
#include <pspiofilemgr.h>
#if ENABLE_LOCAL_MP3
#include <pspmp3.h>
#endif
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_avmodules.h>
#include <psputility_modules.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include "spotify/shannon.h"
#include "spotify/sha1.h"
#include "spotify/dh.h"
#include "spotify/handshake.h"
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
static int16_t audio_output[8192] __attribute__((aligned(64)));
#if ENABLE_LOCAL_MP3
static unsigned char mp3_stream_buffer[64 * 1024] __attribute__((aligned(64)));
static unsigned char mp3_pcm_buffer[16 * (1152 / 2)] __attribute__((aligned(64)));
#endif
static unsigned long codec_data[64] __attribute__((aligned(64)));
static uint32_t sequence;
static int dispatcher_ready, av_loaded, network_ready, unsafe_to_exit, power_locked;
#if ENABLE_LOCAL_MP3
static int mp3_loaded;
#endif
static int dispatcher_init_result, av_load_result, edram_get_result, edram_release_result;
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
static const char *apctl_state_name(int state)
{
    switch (state) {
    case 0: return "DISCONNECTED";
    case 1: return "SCANNING";
    case 2: return "JOINING";
    case 3: return "GETTING_IP";
    case 4: return "GOT_IP";
    default: return "UNKNOWN";
    }
}
static int network_probe(void)
{
    int ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    report_return("network modules", ret);
    if (ret < 0) return ret;
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    report_return("network inet module", ret);
    if (ret < 0) return ret;
    ret = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    report_return("network init", ret);
    if (ret < 0) return ret;
    ret = sceNetInetInit();
    report_return("network inet init", ret);
    if (ret < 0) return ret;
    ret = sceNetResolverInit();
    report_return("network resolver init", ret);
    if (ret < 0) return ret;
    ret = sceNetApctlInit(0x1800, 0x30);
    report_return("network apctl init", ret);
    if (ret < 0) return ret;
    network_ready = 1;
    if (sceWlanGetSwitchState() == 0) {
        pspDebugScreenPrintf("WARNING: WLAN switch is physically OFF!\n");
    }

    int valid_profiles[16];
    int valid_count = 0;

    for (int i = 1; i <= 10; ++i) {
        if (sceUtilityCheckNetParam(i) == 0) {
            netData name, ssid, sec;
            memset(&name, 0, sizeof name);
            memset(&ssid, 0, sizeof ssid);
            memset(&sec, 0, sizeof sec);
            sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &name);
            sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &ssid);
            sceUtilityGetNetParam(i, PSP_NETPARAM_SECURE, &sec);
            pspDebugScreenPrintf("P%d: '%s' (SSID:%s, Sec:%u)\n",
                i, name.asString, ssid.asString, (unsigned int)sec.asUint);
            valid_profiles[valid_count++] = i;
        }
    }

    if (valid_count == 0 && sceUtilityCheckNetParam(0) == 0) {
        valid_profiles[valid_count++] = 0;
        pspDebugScreenPrintf("Found default P0 profile\n");
    }

    if (valid_count == 0) {
        pspDebugScreenPrintf("NO network profile found in PSP settings!\n");
        pspDebugScreenPrintf("Set up Wi-Fi in XMB Network Settings first.\n");
        return -112;
    }

    int connected_profile = -1;
    for (int p = 0; p < valid_count; ++p) {
        int prof = valid_profiles[p];
        pspDebugScreenPrintf("Trying profile %d...\n", prof);
        ret = sceNetApctlConnect(prof);
        if (ret == 0) {
            connected_profile = prof;
            break;
        }
        report_return("Wi-Fi connect failed", ret);
    }

    if (connected_profile < 0) {
        pspDebugScreenPrintf("All profile connects failed!\n");
        pspDebugScreenPrintf("Note: PSP needs 2.4GHz Wi-Fi (WPA2-AES).\n");
        pspDebugScreenPrintf("Check ARK-5 CFW Settings -> WPA2 is Enabled.\n");
        return ret;
    }

    int prev_state = -1;
    for (unsigned int attempt = 0; attempt < 300u; ++attempt) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        ret = sceNetApctlGetState(&state);
        if (ret < 0) {
            report_return("Wi-Fi getState fail", ret);
            return ret;
        }
        if (state != prev_state) {
            pspDebugScreenPrintf("Wi-Fi state: %d (%s) @ tick %u\n",
                state, apctl_state_name(state), attempt);
            prev_state = state;
        }
        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            // Print the assigned IP for diagnostics.
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof info);
            if (sceNetApctlGetInfo(8, &info) >= 0)
                pspDebugScreenPrintf("IP: %s\n", info.ip);

            pspDebugScreenPrintf("Wi-Fi connected on profile %d\n", connected_profile);
            return 0;
        }
        if (state == PSP_NET_APCTL_STATE_DISCONNECTED && attempt > 30u) {
            pspDebugScreenPrintf("Wi-Fi fell back to DISCONNECTED\n");
            return -111;
        }
        sceKernelDelayThread(100000);
    }
    pspDebugScreenPrintf("Wi-Fi TIMEOUT last state: %d (%s)\n",
        prev_state, apctl_state_name(prev_state));
    return -110;
}
#if ENABLE_LOCAL_MP3
static int fill_mp3_stream(SceUID file, int handle)
{
    SceUChar8 *destination;
    SceInt32 available;
    SceInt32 source_position;
    int ret = sceMp3GetInfoToAddStreamData(handle, &destination, &available,
        &source_position);
    if (ret < 0) return ret;
    ret = sceIoLseek32(file, source_position, PSP_SEEK_SET);
    if (ret < 0) return ret;
    const int read = sceIoRead(file, destination, available);
    if (read <= 0) return read;
    ret = sceMp3NotifyAddStreamData(handle, read);
    return ret < 0 ? ret : read;
}
static int dispatch_decoded_pcm(short *decoded, unsigned int sample_count)
{
    if (sample_count == 0 || sample_count > AUDIO_MAX_SAMPLES) return FAIL_PROTOCOL;
    audio_task.magic = TASK_MAGIC;
    audio_task.version = TASK_VERSION;
    audio_task.sample_count = sample_count;
    audio_task.gain_q15 = 32768;
    memcpy((void *)audio_task.samples, decoded, sample_count * sizeof(int16_t));
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
        audio_task.completed_sequence != audio_task.sequence)
        return ret < 0 ? ret : FAIL_PROTOCOL;
    memcpy(audio_output, (const void *)audio_task.samples,
        sample_count * sizeof(int16_t));
    return 0;
}
static int play_arktik_mp3(const char *path)
{
    SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (file < 0) {
        report_return("MP3 open", file);
        return file;
    }
    const SceOff file_end = sceIoLseek(file, 0, PSP_SEEK_END);
    if (file_end <= 0) {
        report_return("MP3 size", (int)file_end);
        sceIoClose(file);
        return FAIL_PATH;
    }
    SceMp3InitArg init;
    memset(&init, 0, sizeof init);
    init.mp3StreamStart = 0;
    init.mp3StreamEnd = (SceInt32)file_end;
    init.mp3Buf = mp3_stream_buffer;
    init.mp3BufSize = sizeof mp3_stream_buffer;
    init.pcmBuf = mp3_pcm_buffer;
    init.pcmBufSize = sizeof mp3_pcm_buffer;
    int ret = sceMp3InitResource();
    if (ret < 0) {
        report_return("MP3 init resource", ret);
        sceIoClose(file);
        return ret;
    }
    int handle = sceMp3ReserveMp3Handle(&init);
    if (handle < 0) {
        report_return("MP3 reserve handle", handle);
        sceMp3TermResource();
        sceIoClose(file);
        return handle;
    }
    int channel = -1;
    int channel_samples = 0;
    int channel_rate = 0;
    int channel_count = 0;
    ret = fill_mp3_stream(file, handle);
    report_return("MP3 initial fill", ret);
    if (ret > 0) {
        ret = sceMp3Init(handle);
        report_return("MP3 init", ret);
    }
    if (ret >= 0) {
        channel_rate = sceMp3GetSamplingRate(handle);
        channel_count = sceMp3GetMp3ChannelNum(handle);
        report_return("MP3 sampling rate", channel_rate);
        report_return("MP3 channels", channel_count);
        if (channel_rate <= 0 || (channel_count != 1 && channel_count != 2))
            ret = FAIL_PROTOCOL;
    }
    while (ret >= 0) {
        if (sceMp3CheckStreamDataNeeded(handle) > 0) {
            ret = fill_mp3_stream(file, handle);
            report_return("MP3 stream refill", ret);
            if (ret <= 0) break;
        }
        short *decoded = NULL;
        const int bytes = sceMp3Decode(handle, &decoded);
        if (bytes < 0 && bytes != (int)0x80671402u)
            report_return("MP3 decode", bytes);
        if (bytes == 0 || bytes == (int)0x80671402u) break;
        if (bytes < 0) {
            ret = bytes;
            break;
        }
        const unsigned int samples = (unsigned int)bytes / sizeof(int16_t);
        const int frames = (int)(samples / (unsigned int)channel_count);
        if (channel < 0) {
            channel = sceAudioSRCChReserve(frames, channel_rate, channel_count);
            channel_samples = frames;
            if (channel < 0) {
                report_return("MP3 audio reserve", channel);
                ret = channel;
                break;
            }
        }
        const int pcm_ret = dispatch_decoded_pcm(decoded, samples);
        if (frames != channel_samples || pcm_ret < 0) {
            report_return("MP3 ME PCM", pcm_ret < 0 ? pcm_ret : FAIL_PROTOCOL);
            ret = FAIL_PROTOCOL;
            break;
        }
        ret = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, audio_output);
        if (ret < 0) report_return("MP3 audio output", ret);
        if (ret < 0) break;
    }
    if (channel >= 0) {
        sceKernelDelayThread(30000);
        sceAudioSRCChRelease();
    }
    const int release_ret = sceMp3ReleaseMp3Handle(handle);
    const int term_ret = sceMp3TermResource();
    sceIoClose(file);
    if (ret >= 0 && release_ret < 0) ret = release_ret;
    if (ret >= 0 && term_ret < 0) ret = term_ret;
    return ret;
}
#endif

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
    const unsigned int block_samples = 1024u;
    int ret = sceAudioSRCChReserve(1024, 44100, 2);
    report_return("audio SRC reserve", ret);
    if (ret < 0) return ret;
    for (unsigned int block = 0; block < block_count; ++block) {
        audio_task.magic = TASK_MAGIC;
        audio_task.version = TASK_VERSION;
        audio_task.sample_count = block_samples;
        audio_task.gain_q15 = 24576;
        for (unsigned int frame = 0; frame < block_samples; ++frame) {
            const unsigned int phase = (block * block_samples + frame) % 100u;
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
        for (unsigned int frame = 0; frame < block_samples; ++frame) {
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

static int run_shannon_probe(void)
{
    shannon_ctx enc, dec;
    static const uint8_t test_key[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    static const char test_msg[] = "Spotify on PSP Media Engine!";
    size_t msg_len = strlen(test_msg);
    uint8_t buffer[64];
    memcpy(buffer, test_msg, msg_len);

    shannon_init(&enc, test_key, 32);
    shannon_init(&dec, test_key, 32);

    shannon_nonce_u32(&enc, 0x12345678U);
    shannon_encrypt(&enc, buffer, msg_len);
    uint8_t mac_enc[4];
    shannon_finish(&enc, mac_enc, 4);

    shannon_nonce_u32(&dec, 0x12345678U);
    shannon_decrypt(&dec, buffer, msg_len);
    uint8_t mac_dec[4];
    shannon_finish(&dec, mac_dec, 4);

    if (memcmp(buffer, test_msg, msg_len) != 0 || memcmp(mac_enc, mac_dec, 4) != 0) {
        pspDebugScreenPrintf("SHANNON CIPHER: FAILED\n");
        return -1;
    }
    pspDebugScreenPrintf("SHANNON CIPHER: PASS (enc/dec/MAC match)\n");
    return 0;
}

static int run_sha1_probe(void)
{
    /* RFC 2202 Test Case 1: Key = 20x 0x0b, Data = "Hi There" */
    uint8_t hmac_key[20];
    memset(hmac_key, 0x0b, 20);
    static const char hmac_data[] = "Hi There";
    uint8_t calculated_mac[20];
    hmac_sha1(hmac_key, 20, (const uint8_t *)hmac_data, 8, calculated_mac);

    static const uint8_t expected_mac[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0xb1, 0xfb, 0xda,
        0x28, 0x00, 0x3e, 0xcf, 0x32, 0x14, 0x5b, 0x0e, 0x9b, 0x97
    };
    if (memcmp(calculated_mac, expected_mac, 20) != 0) {
        pspDebugScreenPrintf("SHA1/HMAC PROBE: FAILED\n");
        return -1;
    }
    pspDebugScreenPrintf("SHA1/HMAC: PASS (RFC 2202 match)\n");
    return 0;
}

static int run_dh_probe(void)
{
    int ret = spotify_dh_selftest();
    if (ret < 0) {
        pspDebugScreenPrintf("DH OAKLEY-1 PROBE: FAILED\n");
        return -1;
    }
    pspDebugScreenPrintf("DH OAKLEY-1: PASS (Alice/Bob match)\n");
    return 0;
}

static int run_spotify_handshake_probe(void)
{
    spotify_session session;
    pspDebugScreenPrintf("--- STARTING SPOTIFY AP HANDSHAKE ---\n");
    int ret = spotify_connect_and_handshake(&session);
    if (ret == 0) {
        pspDebugScreenPrintf("SPOTIFY AP: AUTHENTICATED & READY!\n");
        spotify_disconnect(&session);
    } else {
        pspDebugScreenPrintf("SPOTIFY AP ERROR: %d\n", ret);
    }
    return ret;
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
#if ENABLE_LOCAL_MP3
    stage(34, "Load MP3 module");
    ret = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    report_return("MP3 load", ret);
    if (ret < 0) return ret;
    mp3_loaded = 1;
#endif
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
        run_shannon_probe();
        run_sha1_probe();
        run_dh_probe();
        if (network_probe() < 0)
            pspDebugScreenPrintf("NETWORK probe FAILED\n");
        else
            run_spotify_handshake_probe();
        char arktik_track[512];
        const int track_result = find_arktik_track(arktik_track, sizeof arktik_track);
        report_return("ARKTIK scan", track_result);
        if (track_result == 0) {
            pspDebugScreenPrintf("ARKTIK track: %s\n", arktik_track);
#if ENABLE_LOCAL_MP3
            stage(35, "Decode ARKTIK MP3 through ME");
            report_return("ARKTIK MP3", play_arktik_mp3(arktik_track));
#endif
        }
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
#if ENABLE_LOCAL_MP3
    if (mp3_loaded) report_return("MP3 unload",
        sceUtilityUnloadModule(PSP_MODULE_AV_MP3));
#endif
    if (av_loaded) report_return("AVCODEC unload",
        sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC));
    if (network_ready) {
        report_return("Wi-Fi disconnect", sceNetApctlDisconnect());
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
