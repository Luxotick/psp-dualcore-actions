#include "me_vorbis.h"
#include "../common.h"
#include <me-core-mapper/me-core-mapper.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#include "stb_vorbis.c"

/* Runs on the ME only: no PSP syscalls, libc I/O, logging or $gp (-G0).
 * stb_vorbis decodes from caller-provided memory, so the frame path needs
 * nothing but memcpy/memset and the FPU. */

int me_vorbis_trampoline(void (*fn)(void *), void *arg, void *stack_top);

static uint8_t me_stack[64 * 1024] __attribute__((aligned(64)));

#define LINE_DOWN(p) ((void *)((uintptr_t)(p) & ~(uintptr_t)63))
#define LINE_SPAN(p, n) ((((uintptr_t)(p) & 63u) + (uint32_t)(n) + 63u) & ~63u)

static int16_t to_s16(float x)
{
    int v = (int)(x * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void decode_job(void *param)
{
    MeVorbisJob *job = param;
    meCoreDcacheInvalidateRange(job, sizeof *job);
    shared_sync();
    if (job->magic != TASK_MAGIC || job->version != TASK_VERSION ||
        job->state != ME_VORBIS_READY || job->sequence == 0 || job->data_len <= 0) {
        job->state = ME_VORBIS_ERROR;
        return;
    }
    job->state = ME_VORBIS_RUNNING;

    if (job->first)
        meCoreDcacheInvalidateRange(job->decoder_memory, job->decoder_memory_size);
    meCoreDcacheInvalidateRange(LINE_DOWN(job->data), LINE_SPAN(job->data, job->data_len));

    int channels = 0, samples = 0;
    float **out = 0;
    int used = stb_vorbis_decode_frame_pushdata((stb_vorbis *)job->decoder, job->data,
                                                job->data_len, &channels, &out, &samples);
    if (samples > (int)job->pcm_frames_cap) samples = (int)job->pcm_frames_cap;
    int16_t *pcm = job->pcm;
    for (int i = 0; i < samples; ++i) {
        int16_t l = to_s16(out[0][i]);
        pcm[i * 2] = l;
        pcm[i * 2 + 1] = channels > 1 ? to_s16(out[1][i]) : l;
    }
    if (samples > 0) meCoreDcacheWritebackRange(pcm, LINE_SPAN(pcm, samples * 4));

    job->used = used;
    job->frames = samples;
    job->channels = channels;
    job->completed_sequence = job->sequence;
    shared_sync();
    job->state = ME_VORBIS_DONE;
}

__attribute__((noinline, aligned(64)))
void me_vorbis_task(void *param)
{
    if (param == 0) return;
    me_vorbis_trampoline(decode_job, param, me_stack + sizeof me_stack);
    /* Publish results before firmware clears its busy flags. */
    shared_sync();
    meCoreDcacheWritebackRange(param, sizeof(MeVorbisJob));
    shared_sync();
}
