#define ENABLE_LOCAL_MP3 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PSP_HTONS
#define PSP_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))
#endif
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
#include "spotify/config.h"
#include "spotify/login.h"
#include "spotify/audiokey.h"
#include "spotify/stream.h"
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
#if ENABLE_LOCAL_MP3
static SharedAudioTask audio_task __attribute__((aligned(64)));
static int16_t audio_output[8192] __attribute__((aligned(64)));
static unsigned char mp3_stream_buffer[64 * 1024] __attribute__((aligned(64)));
static unsigned char mp3_pcm_buffer[16 * (1152 / 2)] __attribute__((aligned(64)));
static uint32_t sequence;
#endif
static unsigned long codec_data[64] __attribute__((aligned(64)));
static int dispatcher_ready, av_loaded, network_ready, unsafe_to_exit, power_locked;
#if ENABLE_LOCAL_MP3
static int mp3_loaded;
#endif
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
#if 0
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
#endif
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
        pspDebugScreenPrintf("WARNING: WLAN switch is physically OFF!\n");
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
        pspDebugScreenPrintf("Wi-Fi: No profile found in PSP settings!\n");
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
        pspDebugScreenPrintf("Wi-Fi: Connect failed!\n");
        return ret;
    }

    pspDebugScreenPrintf("Wi-Fi: Connecting (Profile %d)...", connected_profile);

    for (unsigned int attempt = 0; attempt < 300u; ++attempt) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        ret = sceNetApctlGetState(&state);
        if (ret < 0) return ret;

        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof info);
            if (sceNetApctlGetInfo(8, &info) >= 0) {
                pspDebugScreenPrintf(" OK! (IP: %s)\n", info.ip);
            } else {
                pspDebugScreenPrintf(" OK!\n");
            }
            return 0;
        }
        if (state == PSP_NET_APCTL_STATE_DISCONNECTED && attempt > 40u) {
            pspDebugScreenPrintf(" Disconnected\n");
            return -111;
        }
        sceKernelDelayThread(100000);
    }
    pspDebugScreenPrintf(" Timeout\n");
    return -110;
}
#if ENABLE_LOCAL_MP3
static int __attribute__((unused)) fill_mp3_stream(SceUID file, int handle)
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
static int __attribute__((unused)) play_arktik_mp3(const char *path)
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

static int play_spotify_live_stream(const char *hostname, const char *url_path)
{
    pspDebugScreenPrintf("STREAM: Resolving %s...\n", hostname);
    int rid = 0;
    char res_buf[1024];
    int ret = sceNetResolverCreate(&rid, res_buf, sizeof(res_buf));
    if (ret < 0) {
        report_return("Resolver create", ret);
        return ret;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = PSP_HTONS(80);

    ret = sceNetResolverStartNtoA(rid, hostname, &server_addr.sin_addr, 5, 3);
    sceNetResolverDelete(rid);
    if (ret < 0) {
        report_return("DNS resolve", ret);
        return ret;
    }

    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        report_return("Socket create", sock);
        return sock;
    }

    pspDebugScreenPrintf("STREAM: Connecting to %s:80...\n", hostname);
    ret = sceNetInetConnect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        report_return("Connect", ret);
        sceNetInetClose(sock);
        return ret;
    }

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: PSP-Spotify/1.0\r\n"
             "Connection: close\r\n\r\n",
             url_path, hostname);

    ret = sceNetInetSend(sock, req, strlen(req), 0);
    if (ret < 0) {
        report_return("Send HTTP", ret);
        sceNetInetClose(sock);
        return ret;
    }

    /* Read HTTP headers until \r\n\r\n */
    char hdr_buf[2048];
    size_t hdr_bytes = 0;
    char *body_start = NULL;
    size_t initial_body_len = 0;

    while (hdr_bytes < sizeof(hdr_buf) - 1) {
        int r = sceNetInetRecv(sock, hdr_buf + hdr_bytes, 1, 0);
        if (r <= 0) break;
        hdr_bytes += (size_t)r;
        hdr_buf[hdr_bytes] = '\0';
        body_start = strstr(hdr_buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            initial_body_len = hdr_bytes - (size_t)(body_start - hdr_buf);
            break;
        }
    }

    if (!body_start) {
        pspDebugScreenPrintf("STREAM: Bad HTTP response!\n");
        sceNetInetClose(sock);
        return FAIL_PROTOCOL;
    }

    /* Parse Content-Length if present */
    size_t content_length = 0;
    const char *cl_pos = strstr(hdr_buf, "Content-Length:");
    if (!cl_pos) cl_pos = strstr(hdr_buf, "content-length:");
    if (cl_pos) {
        content_length = (size_t)strtoul(cl_pos + 15, NULL, 10);
    }
    if (content_length == 0) {
        content_length = 512 * 1024;
    }

    pspDebugScreenPrintf("STREAM: HTTP 200 OK (%u KB)\n", (unsigned int)(content_length / 1024));

    /* Allocate RAM streaming buffer */
    size_t ram_capacity = content_length + 16384;
    uint8_t *ram_stream = (uint8_t *)malloc(ram_capacity);
    if (!ram_stream) {
        pspDebugScreenPrintf("STREAM: Out of RAM!\n");
        sceNetInetClose(sock);
        return FAIL_MEMORY;
    }

    size_t ram_buffered = 0;
    if (initial_body_len > 0) {
        memcpy(ram_stream, body_start, initial_body_len);
        ram_buffered += initial_body_len;
    }

    /* Pre-buffer: fill initial 48 KB (or entire stream if smaller) before starting audio */
    pspDebugScreenPrintf("STREAM: Pre-buffering ");
    size_t prebuffer_target = (content_length < 49152) ? content_length : 49152;
    while (ram_buffered < prebuffer_target) {
        int r = sceNetInetRecv(sock, ram_stream + ram_buffered, 4096, 0);
        if (r <= 0) break;
        ram_buffered += (size_t)r;
        pspDebugScreenPrintf(".");
    }
    pspDebugScreenPrintf(" OK! (%u KB in RAM)\n", (unsigned int)(ram_buffered / 1024));

    /* Initialize PSP MP3 decoder resource */
    SceMp3InitArg init;
    memset(&init, 0, sizeof init);
    init.mp3StreamStart = 0;
    init.mp3StreamEnd = (SceInt32)content_length;
    init.mp3Buf = mp3_stream_buffer;
    init.mp3BufSize = sizeof mp3_stream_buffer;
    init.pcmBuf = mp3_pcm_buffer;
    init.pcmBufSize = sizeof mp3_pcm_buffer;

    ret = sceMp3InitResource();
    if (ret < 0) {
        report_return("MP3 init resource", ret);
        free(ram_stream);
        sceNetInetClose(sock);
        return ret;
    }

    int handle = sceMp3ReserveMp3Handle(&init);
    if (handle < 0) {
        report_return("MP3 reserve handle", handle);
        sceMp3TermResource();
        free(ram_stream);
        sceNetInetClose(sock);
        return handle;
    }

    /* Initial fill into sceMp3 internal ring buffer */
    SceUChar8 *dest = NULL;
    SceInt32 avail = 0;
    SceInt32 src_pos = 0;
    ret = sceMp3GetInfoToAddStreamData(handle, &dest, &avail, &src_pos);
    if (ret >= 0 && avail > 0 && src_pos < (SceInt32)ram_buffered) {
        int to_copy = (avail < (SceInt32)(ram_buffered - (size_t)src_pos)) ? avail : (SceInt32)(ram_buffered - (size_t)src_pos);
        memcpy(dest, ram_stream + src_pos, (size_t)to_copy);
        ret = sceMp3NotifyAddStreamData(handle, to_copy);
    }

    if (ret > 0) {
        ret = sceMp3Init(handle);
        report_return("MP3 init", ret);
    }

    int channel = -1;
    int channel_samples = 0;
    int channel_rate = 0;
    int channel_count = 0;
    if (ret >= 0) {
        channel_rate = sceMp3GetSamplingRate(handle);
        channel_count = sceMp3GetMp3ChannelNum(handle);
        report_return("MP3 sampling rate", channel_rate);
        report_return("MP3 channels", channel_count);
        if (channel_rate <= 0 || (channel_count != 1 && channel_count != 2))
            ret = FAIL_PROTOCOL;
    }

    pspDebugScreenPrintf("--- LIVE STREAMING TO MEDIA ENGINE ---\n");

    /* Live playback loop */
    while (ret >= 0) {
        if (sceMp3CheckStreamDataNeeded(handle) > 0) {
            ret = sceMp3GetInfoToAddStreamData(handle, &dest, &avail, &src_pos);
            if (ret < 0) break;

            while (src_pos + avail > (SceInt32)ram_buffered && sock >= 0) {
                size_t space_left = ram_capacity - ram_buffered;
                if (space_left == 0) break;
                size_t fetch_chunk = space_left > 8192 ? 8192 : space_left;
                int r = sceNetInetRecv(sock, ram_stream + ram_buffered, fetch_chunk, 0);
                if (r <= 0) {
                    sceNetInetClose(sock);
                    sock = -1;
                    break;
                }
                ram_buffered += (size_t)r;
            }

            int have_bytes = (SceInt32)ram_buffered - src_pos;
            if (have_bytes > 0) {
                int to_copy = avail < have_bytes ? avail : have_bytes;
                memcpy(dest, ram_stream + src_pos, (size_t)to_copy);
                ret = sceMp3NotifyAddStreamData(handle, to_copy);
                if (ret < 0) break;
            } else if (sock < 0) {
                break;
            }
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
    if (sock >= 0) sceNetInetClose(sock);
    free(ram_stream);

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
#if 0 /* disabled – verbose stage logging */
static void stage(unsigned int number, const char *message)
{
    pspDebugScreenPrintf("[%02u] %s\n", number, message);
    sceDisplayWaitVblankStart();
}
#endif
static void report_return(const char *operation, int ret)
{
    pspDebugScreenPrintf("%s: 0x%08X (%d)\n", operation, (unsigned int)ret, ret);
}
#if 0 /* disabled – ME arithmetic + audio hardware probes */
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
#endif /* disabled – ME arithmetic + audio hardware probes */

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
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
        0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00
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

        spotify_config cfg;
        if (spotify_config_load(&cfg) == 0) {
            ret = spotify_login(&session, &cfg);
            if (ret == 0) {
                /* Track: Kayra - Bagisla (spotify:track:32sfCZ7VPQrnRwBWQKN1Yg) */
                static const uint8_t file_id[20] = {
                    0x8f, 0x29, 0x36, 0x47, 0x40, 0x92, 0x8c, 0x96, 0x1c, 0xec,
                    0xca, 0x96, 0xf7, 0xed, 0xf1, 0xb6, 0x36, 0x69, 0x55, 0xe9
                };
                static const uint8_t track_gid[16] = {
                    0x63, 0xdf, 0x38, 0x40, 0xd0, 0x2a, 0x4b, 0x56,
                    0xa9, 0xf0, 0x6a, 0xd8, 0x37, 0xfc, 0x51, 0x74
                };
                uint8_t aes_key[16];
                memset(aes_key, 0, sizeof(aes_key));

                pspDebugScreenPrintf("\n--- REQUESTING TRACK: Kayra - Bagisla ---\n");
                int key_ret = spotify_request_audio_key(&session, file_id, track_gid, aes_key);
                (void)key_ret;

                /* Stream audio directly from Spotify CDN in RAM (Live Stream) */
                pspDebugScreenPrintf("\n--- LIVE STREAM: Kayra - Bagisla ---\n");
#if ENABLE_LOCAL_MP3
                play_spotify_live_stream("p.scdn.co",
                    "/mp3-preview/8f29364740928c961cecca96f7edf1b6366955e9");
#endif
            }
        } else {
            pspDebugScreenPrintf("Notice: Place spotify.cfg on Memory Stick to login!\n");
        }

        spotify_disconnect(&session);
    } else {
        pspDebugScreenPrintf("SPOTIFY AP ERROR: %d\n", ret);
    }
    return ret;
}

#if 0 /* disabled – was ME arithmetic test UI */
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
#endif
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
#if ENABLE_LOCAL_MP3
    ret = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if (ret < 0) return ret;
    mp3_loaded = 1;
#endif
    sceKernelDcacheWritebackAll();
    ret = sceAudiocodecGetEDRAM(codec_data, 0x1000);
    if (ret < 0) return ret;
    ret = sceAudiocodecReleaseEDRAM(codec_data);
    if (ret < 0) return ret;
    unsafe_to_exit = 0;
    pspDebugScreenPrintf("PSP-3000 (model %d) ME OK\n", device.model);
    return 0;
}
int main(int argc, char **argv)
{
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    int init_result = set_application_directory(argc, argv);
    if (init_result >= 0) {
        init_result = sceKernelCreateCallback("ME proof exit", exit_callback, NULL);
        if (init_result >= 0) {
            init_result = sceKernelRegisterExitCallback(init_result);
        }
    }
    if (init_result >= 0) init_result = initialize();
    if (init_result < 0) {
        pspDebugScreenPrintf("INIT FAIL: 0x%08X\n", (unsigned int)init_result);
    } else {
        /* Crypto self-tests (offline, no network needed) */
        run_shannon_probe();
        run_sha1_probe();
        run_dh_probe();
        /* Network + Spotify AP handshake */
        if (network_probe() < 0)
            pspDebugScreenPrintf("NETWORK FAILED\n");
        else
            run_spotify_handshake_probe();
    }
    uint32_t previous_buttons = 0;
controls:
    for (;;) {
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
            pspDebugScreenPrintf("ME shutdown failed. START=reboot.\n");
            unsafe_to_exit = 1;
            exit_requested = 0;
            goto controls;
        }
    }
#if ENABLE_LOCAL_MP3
    if (mp3_loaded) sceUtilityUnloadModule(PSP_MODULE_AV_MP3);
#endif
    if (av_loaded) sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
    if (network_ready) {
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
