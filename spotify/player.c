#include "player.h"
#include "audiodecrypt.h"
#include "http.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspthreadman.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#include "stb_vorbis.c"

/* Decrypted Spotify Ogg files start with a 0xa7-byte custom header page. */
#define SPOTIFY_OGG_HEADER 0xa7
#define BLOCK_FRAMES 1024
#define OUTPUT_SLOTS 4
#define FETCH_CHUNK 16384

/* Each slot is played by audio DMA after OutputBlocking returns, so the next
 * block is written into a different slot (one shared buffer crackled). */
static int16_t output_ring[OUTPUT_SLOTS][BLOCK_FRAMES * 2] __attribute__((aligned(64)));
/* All stb_vorbis setup and decode memory; no malloc or alloca. */
static char vorbis_memory[384 * 1024] __attribute__((aligned(64)));

typedef struct {
    http_stream http;
    audio_decrypt decrypt;
    uint8_t *buf;
    size_t capacity;
    volatile size_t buffered;   /* decrypted bytes available */
    volatile int open;          /* fetch thread still running */
    volatile int stop;
} fetcher;

static int fetch_thread(SceSize args, void *argp)
{
    (void)args;
    fetcher *f = *(fetcher **)argp;
    while (!f->stop && f->buffered < f->capacity) {
        size_t space = f->capacity - f->buffered;
        size_t chunk = space > FETCH_CHUNK ? FETCH_CHUNK : space;
        uint8_t *dst = f->buf + f->buffered;
        int r = http_stream_read(&f->http, dst, (unsigned int)chunk);
        if (r <= 0) break;
        /* Publish only decrypted bytes. */
        audio_decrypt_run(&f->decrypt, dst, (size_t)r);
        f->buffered += (size_t)r;
    }
    f->open = 0;
    return 0;
}

static int16_t to_s16(float x)
{
    int v = (int)(x * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

int spotify_play_ogg(const char *urls, unsigned int url_stride, int url_count,
                     const uint8_t key[16])
{
    static fetcher f;
    memset(&f, 0, sizeof f);
    f.http.sock = -1;
    uint64_t content_length = 0;
    int status = -1;
    log_printf("PLAYER: opening full file (%d CDN URLs)\n", url_count);
    for (int u = 0; u < url_count && status != 200; ++u) {
        status = http_stream_open(&f.http, urls + (size_t)u * url_stride, &content_length);
        if (status > 0 && status != 200) {
            log_printf("PLAYER: CDN %d HTTP %d\n", u, status);
            http_stream_close(&f.http);
        }
    }
    if (status != 200 || content_length <= SPOTIFY_OGG_HEADER || content_length > 32u * 1024 * 1024) {
        log_printf("PLAYER: no usable CDN response (%d, %u bytes)\n", status,
                   (unsigned int)content_length);
        if (status > 0) http_stream_close(&f.http);
        return -1;
    }
    f.capacity = (size_t)content_length;
    f.buf = malloc(f.capacity);
    if (!f.buf) {
        log_printf("PLAYER: cannot allocate %u KB\n", (unsigned int)(f.capacity / 1024));
        http_stream_close(&f.http);
        return -2;
    }
    audio_decrypt_init(&f.decrypt, key);
    f.open = 1;
    log_printf("PLAYER: %u KB Ogg Vorbis file\n", (unsigned int)(f.capacity / 1024));

    /* Decoding headroom: run the main CPU at full speed while playing. */
    scePowerSetClockFrequency(333, 333, 166);

    fetcher *fp = &f;
    SceUID thid = sceKernelCreateThread("ogg_fetch", fetch_thread, 0x30, 0x10000, 0, NULL);
    if (thid < 0 || sceKernelStartThread(thid, sizeof fp, &fp) < 0) {
        log_printf("PLAYER: fetch thread failed 0x%08X\n", (unsigned int)thid);
        if (thid >= 0) sceKernelDeleteThread(thid);
        http_stream_close(&f.http);
        free(f.buf);
        return -3;
    }

    int ret = 0;
    stb_vorbis *v = NULL;
    stb_vorbis_alloc alloc = { vorbis_memory, (int)sizeof vorbis_memory };
    size_t pos = SPOTIFY_OGG_HEADER;
    int channel = -1;
    unsigned int underruns = 0;

    /* Open once enough of the headers (comment + codebooks) has arrived. */
    for (;;) {
        size_t have = f.buffered;
        if (have > pos) {
            int used = 0, error = 0;
            v = stb_vorbis_open_pushdata(f.buf + pos, (int)(have - pos), &used, &error, &alloc);
            if (v) { pos += (size_t)used; break; }
            if (error != VORBIS_need_more_data) {
                log_printf("PLAYER: Vorbis open error %d\n", error);
                ret = -4;
                goto out;
            }
        }
        if (!f.open && f.buffered == have) {
            log_printf("PLAYER: stream ended before Vorbis headers\n");
            ret = -5;
            goto out;
        }
        sceKernelDelayThread(10000);
    }

    stb_vorbis_info info = stb_vorbis_get_info(v);
    log_printf("PLAYER: %u Hz, %d ch, decoder memory %u KB\n", info.sample_rate, info.channels,
               (unsigned int)((info.setup_memory_required + info.temp_memory_required) / 1024));
    if (info.channels < 1 || info.channels > 2) { ret = -6; goto out; }

    channel = sceAudioSRCChReserve(BLOCK_FRAMES, (int)info.sample_rate, 2);
    if (channel < 0) {
        log_printf("PLAYER: audio reserve 0x%08X\n", (unsigned int)channel);
        ret = channel;
        goto out;
    }
    log_printf("--- PLAYING FULL TRACK ---\n");

    unsigned int slot = 0, fill = 0;
    unsigned long long frames_played = 0, next_report = 10ull * info.sample_rate;
    SceInt64 started = sceKernelGetSystemTimeWide();
    for (;;) {
        size_t have = f.buffered;
        int channels = 0, samples = 0;
        float **out = NULL;
        int used = have > pos
            ? stb_vorbis_decode_frame_pushdata(v, f.buf + pos, (int)(have - pos),
                                               &channels, &out, &samples)
            : 0;
        if (used == 0 && samples == 0) {
            if (!f.open && f.buffered == have) break;   /* end of file */
            ++underruns;
            sceKernelDelayThread(5000);
            continue;
        }
        pos += (size_t)used;

        for (int i = 0; i < samples; ++i) {
            int16_t l = to_s16(out[0][i]);
            int16_t r = channels > 1 ? to_s16(out[1][i]) : l;
            output_ring[slot][fill * 2] = l;
            output_ring[slot][fill * 2 + 1] = r;
            if (++fill == BLOCK_FRAMES) {
                int ar = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, output_ring[slot]);
                if (ar < 0) {
                    log_printf("PLAYER: audio output 0x%08X\n", (unsigned int)ar);
                    ret = ar;
                    goto out;
                }
                slot = (slot + 1) % OUTPUT_SLOTS;
                fill = 0;
            }
        }
        frames_played += (unsigned long long)samples;
        if (frames_played >= next_report) {
            next_report += 10ull * info.sample_rate;
            log_printf("PLAYER: %u s played, %u/%u KB fetched, %u waits, %u ms wall\n",
                       (unsigned int)(frames_played / info.sample_rate),
                       (unsigned int)(f.buffered / 1024), (unsigned int)(f.capacity / 1024),
                       underruns,
                       (unsigned int)((sceKernelGetSystemTimeWide() - started) / 1000));
        }
    }
    if (fill > 0) {
        memset(&output_ring[slot][fill * 2], 0, (BLOCK_FRAMES - fill) * 2 * sizeof(int16_t));
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, output_ring[slot]);
    }
    log_printf("PLAYER: finished, %u s, %u waits\n",
               (unsigned int)(frames_played / info.sample_rate), underruns);

out:
    if (channel >= 0) {
        sceKernelDelayThread(50000);
        sceAudioSRCChRelease();
    }
    if (v) stb_vorbis_close(v);
    f.stop = 1;
    SceUInt timeout = 20u * 1000u * 1000u;
    if (sceKernelWaitThreadEnd(thid, &timeout) < 0) sceKernelTerminateThread(thid);
    sceKernelDeleteThread(thid);
    http_stream_close(&f.http);
    free(f.buf);
    scePowerSetClockFrequency(222, 222, 111);
    return ret;
}
