#include "sha1.h"
#include <string.h>

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }

    for (int i = 16; i < 80; ++i) {
        w[i] = SHA1_ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    /* Round 1 */
    for (int i = 0; i < 20; ++i) {
        uint32_t temp = SHA1_ROTL(a, 5) + ((b & c) | ((~b) & d)) + e + 0x5A827999U + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = temp;
    }

    /* Round 2 */
    for (int i = 20; i < 40; ++i) {
        uint32_t temp = SHA1_ROTL(a, 5) + (b ^ c ^ d) + e + 0x6ED9EBA1U + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = temp;
    }

    /* Round 3 */
    for (int i = 40; i < 60; ++i) {
        uint32_t temp = SHA1_ROTL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + 0x8F1BBCDCU + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = temp;
    }

    /* Round 4 */
    for (int i = 60; i < 80; ++i) {
        uint32_t temp = SHA1_ROTL(a, 5) + (b ^ c ^ d) + e + 0xCA62C1D6U + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t buffer_idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    size_t input_idx = 0;
    if (buffer_idx > 0) {
        size_t needed = 64 - buffer_idx;
        if (len < needed) {
            memcpy(&ctx->buffer[buffer_idx], data, len);
            return;
        }
        memcpy(&ctx->buffer[buffer_idx], data, needed);
        sha1_transform(ctx->state, ctx->buffer);
        input_idx = needed;
    }

    while (input_idx + 64 <= len) {
        sha1_transform(ctx->state, &data[input_idx]);
        input_idx += 64;
    }

    if (input_idx < len) {
        memcpy(ctx->buffer, &data[input_idx], len - input_idx);
    }
}

void sha1_final(sha1_ctx *ctx, uint8_t digest[20])
{
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_idx = (size_t)(ctx->count & 0x3F);

    ctx->buffer[buffer_idx++] = 0x80;
    if (buffer_idx > 56) {
        memset(&ctx->buffer[buffer_idx], 0, 64 - buffer_idx);
        sha1_transform(ctx->state, ctx->buffer);
        buffer_idx = 0;
    }
    memset(&ctx->buffer[buffer_idx], 0, 56 - buffer_idx);

    for (int i = 0; i < 8; ++i) {
        ctx->buffer[56 + i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    }
    sha1_transform(ctx->state, ctx->buffer);

    for (int i = 0; i < 5; ++i) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               uint8_t out[20])
{
    uint8_t k_pad[64];
    uint8_t key_hashed[20];

    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 64) {
        sha1_ctx ctx;
        sha1_init(&ctx);
        sha1_update(&ctx, key, key_len);
        sha1_final(&ctx, key_hashed);
        memcpy(k_pad, key_hashed, 20);
    } else {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    sha1_ctx inner_ctx;
    uint8_t inner_hash[20];
    sha1_init(&inner_ctx);
    sha1_update(&inner_ctx, ipad, 64);
    sha1_update(&inner_ctx, data, data_len);
    sha1_final(&inner_ctx, inner_hash);

    sha1_ctx outer_ctx;
    sha1_init(&outer_ctx);
    sha1_update(&outer_ctx, opad, 64);
    sha1_update(&outer_ctx, inner_hash, 20);
    sha1_final(&outer_ctx, out);
}
