#ifndef SPOTIFY_ME_VORBIS_H
#define SPOTIFY_ME_VORBIS_H

#include <stdint.h>

/* One stb_vorbis frame decode executed on the Media Engine.
 *
 * Ownership: the SC fills a READY job and hands it over with a D-cache
 * writeback; the ME invalidates its view of the job and of the input window,
 * decodes, converts to interleaved stereo s16 in `pcm`, writes both back and
 * publishes DONE. After setup (done on the SC) the decoder memory belongs to
 * the ME only: `first` makes the ME drop stale cache lines for it once. */

#define ME_VORBIS_MAX_FRAMES 4096u

#define ME_VORBIS_READY   1u
#define ME_VORBIS_RUNNING 2u
#define ME_VORBIS_DONE    3u
#define ME_VORBIS_ERROR   4u

typedef struct __attribute__((aligned(64))) MeVorbisJob {
    volatile uint32_t magic, version, state, sequence, completed_sequence;
    volatile uint32_t first;
    void *decoder;                  /* stb_vorbis * inside decoder_memory */
    void *decoder_memory;
    uint32_t decoder_memory_size;
    const uint8_t *data;            /* input window, SC-written and flushed */
    int32_t data_len;
    int16_t *pcm;                   /* 64-byte aligned output */
    uint32_t pcm_frames_cap;
    volatile int32_t used, frames, channels;
} MeVorbisJob;

/* ME entry point (Task.func); never called on the SC. */
void me_vorbis_task(void *param);

#endif /* SPOTIFY_ME_VORBIS_H */
