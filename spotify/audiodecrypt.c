#include "audiodecrypt.h"
#include <string.h>

static const uint8_t AUDIO_IV[16] = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93
};

void audio_decrypt_init(audio_decrypt *d, const uint8_t key[16])
{
    br_aes_ct_ctr_init(&d->keys, key, 16);
    d->counter = ((uint32_t)AUDIO_IV[12] << 24) | ((uint32_t)AUDIO_IV[13] << 16) |
                 ((uint32_t)AUDIO_IV[14] << 8) | (uint32_t)AUDIO_IV[15];
    d->keystream_used = 16;
}

void audio_decrypt_run(audio_decrypt *d, uint8_t *buf, size_t len)
{
    /* Finish a block left partially used by the previous call. */
    while (len > 0 && d->keystream_used < 16) {
        *buf++ ^= d->keystream[d->keystream_used++];
        --len;
    }
    /* Whole blocks straight through BearSSL. */
    size_t whole = len & ~(size_t)15;
    if (whole) {
        d->counter = br_aes_ct_ctr_run(&d->keys, AUDIO_IV, d->counter, buf, whole);
        buf += whole;
        len -= whole;
    }
    /* Keep the keystream of a trailing partial block for the next call. */
    if (len > 0) {
        memset(d->keystream, 0, sizeof d->keystream);
        d->counter = br_aes_ct_ctr_run(&d->keys, AUDIO_IV, d->counter, d->keystream, 16);
        d->keystream_used = 0;
        while (len > 0) {
            *buf++ ^= d->keystream[d->keystream_used++];
            --len;
        }
    }
}
