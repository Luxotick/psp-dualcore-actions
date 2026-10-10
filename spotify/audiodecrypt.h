#ifndef SPOTIFY_AUDIODECRYPT_H
#define SPOTIFY_AUDIODECRYPT_H

#include <bearssl.h>
#include <stdint.h>
#include <stddef.h>

/* Spotify audio files are AES-128-CTR encrypted with a fixed IV; the
 * counter is the IV taken as a 128-bit big-endian integer plus the block
 * index. Its low 32 bits start at 0x3f630d93, so the carry into the upper
 * 96 bits only happens after ~51 GB and BearSSL's 32-bit counter suffices. */
typedef struct {
    br_aes_ct_ctr_keys keys;
    uint32_t counter;
    uint8_t keystream[16];
    unsigned int keystream_used;
} audio_decrypt;

/* Starts decryption at byte offset 0 of the file. */
void audio_decrypt_init(audio_decrypt *d, const uint8_t key[16]);
/* Decrypts the next len bytes of the file in place. */
void audio_decrypt_run(audio_decrypt *d, uint8_t *buf, size_t len);

#endif /* SPOTIFY_AUDIODECRYPT_H */
