#include "shannon.h"
#include <string.h>

#define SHANNON_INITKONST 0x6996c53aU
#define SHANNON_KEYP      13

static inline uint32_t rotl32(uint32_t w, unsigned int x)
{
    return (w << x) | (w >> (32 - x));
}

static inline uint32_t sbox1(uint32_t w)
{
    w ^= rotl32(w, 5) | rotl32(w, 7);
    w ^= rotl32(w, 19) | rotl32(w, 22);
    return w;
}

static inline uint32_t sbox2(uint32_t w)
{
    w ^= rotl32(w, 7) | rotl32(w, 22);
    w ^= rotl32(w, 5) | rotl32(w, 19);
    return w;
}

static void shannon_cycle(shannon_ctx *c)
{
    uint32_t t = c->R[12] ^ c->R[13] ^ c->konst;
    t = sbox1(t) ^ rotl32(c->R[0], 1);
    for (int i = 1; i < 16; ++i) {
        c->R[i - 1] = c->R[i];
    }
    c->R[15] = t;
    t = sbox2(c->R[2] ^ c->R[15]);
    c->R[0] ^= t;
    c->sbuf = t ^ c->R[8] ^ c->R[12];
}

static void shannon_diffuse(shannon_ctx *c)
{
    for (int i = 0; i < 16; ++i) {
        shannon_cycle(c);
    }
}

static void shannon_loadkey(shannon_ctx *c, const uint8_t *key, size_t keylen)
{
    size_t offset = 0;
    while (offset < keylen) {
        size_t chunk = keylen - offset;
        uint32_t word = 0;
        if (chunk >= 4) {
            word = (uint32_t)key[offset] |
                   ((uint32_t)key[offset + 1] << 8) |
                   ((uint32_t)key[offset + 2] << 16) |
                   ((uint32_t)key[offset + 3] << 24);
            offset += 4;
        } else {
            for (size_t i = 0; i < chunk; ++i) {
                word |= ((uint32_t)key[offset + i]) << (8 * i);
            }
            offset += chunk;
        }
        c->R[SHANNON_KEYP] ^= word;
        shannon_cycle(c);
    }
    c->R[SHANNON_KEYP] ^= (uint32_t)keylen;
    shannon_cycle(c);
    for (int i = 0; i < 16; ++i) {
        c->CRC[i] = c->R[i];
    }
    shannon_diffuse(c);
    for (int i = 0; i < 16; ++i) {
        c->R[i] ^= c->CRC[i];
    }
}

static void shannon_crcfunc(shannon_ctx *c, uint32_t i)
{
    uint32_t t = c->CRC[0] ^ c->CRC[2] ^ c->CRC[15] ^ i;
    for (int j = 1; j < 16; ++j) {
        c->CRC[j - 1] = c->CRC[j];
    }
    c->CRC[15] = t;
}

static void shannon_macfunc(shannon_ctx *c, uint32_t i)
{
    shannon_crcfunc(c, i);
    c->R[SHANNON_KEYP] ^= i;
}

void shannon_init(shannon_ctx *c, const uint8_t *key, size_t keylen)
{
    memset(c, 0, sizeof(*c));
    c->konst = SHANNON_INITKONST;
    c->R[0] = 1;
    c->R[1] = 1;
    for (int i = 2; i < 16; ++i) {
        c->R[i] = c->R[i - 1] + c->R[i - 2];
    }
    shannon_loadkey(c, key, keylen);
    c->konst = c->R[0];
    for (int i = 0; i < 16; ++i) {
        c->initR[i] = c->R[i];
    }
}

void shannon_nonce(shannon_ctx *c, const uint8_t *nonce, size_t noncelen)
{
    for (int i = 0; i < 16; ++i) {
        c->R[i] = c->initR[i];
    }
    c->konst = SHANNON_INITKONST;
    shannon_loadkey(c, nonce, noncelen);
    c->konst = c->R[0];
    c->nbuf = 0;
}

void shannon_nonce_u32(shannon_ctx *c, uint32_t nonce)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(nonce >> 24);
    buf[1] = (uint8_t)(nonce >> 16);
    buf[2] = (uint8_t)(nonce >> 8);
    buf[3] = (uint8_t)nonce;
    shannon_nonce(c, buf, 4);
}

void shannon_encrypt(shannon_ctx *c, uint8_t *buf, size_t len)
{
    size_t idx = 0;

    while (c->nbuf > 0 && idx < len) {
        uint8_t b = buf[idx];
        c->mbuf ^= ((uint32_t)b) << (32 - c->nbuf);
        buf[idx] = (uint8_t)(b ^ ((c->sbuf >> (32 - c->nbuf)) & 0xFF));
        idx++;
        c->nbuf -= 8;
        if (c->nbuf == 0) {
            shannon_macfunc(c, c->mbuf);
        }
    }

    while (idx + 4 <= len) {
        shannon_cycle(c);
        uint32_t word = (uint32_t)buf[idx] |
                        ((uint32_t)buf[idx + 1] << 8) |
                        ((uint32_t)buf[idx + 2] << 16) |
                        ((uint32_t)buf[idx + 3] << 24);
        shannon_macfunc(c, word);
        word ^= c->sbuf;
        buf[idx]     = (uint8_t)word;
        buf[idx + 1] = (uint8_t)(word >> 8);
        buf[idx + 2] = (uint8_t)(word >> 16);
        buf[idx + 3] = (uint8_t)(word >> 24);
        idx += 4;
    }

    if (idx < len) {
        shannon_cycle(c);
        c->mbuf = 0;
        c->nbuf = 32;
        while (idx < len) {
            uint8_t b = buf[idx];
            c->mbuf ^= ((uint32_t)b) << (32 - c->nbuf);
            buf[idx] = (uint8_t)(b ^ ((c->sbuf >> (32 - c->nbuf)) & 0xFF));
            idx++;
            c->nbuf -= 8;
        }
    }
}

void shannon_decrypt(shannon_ctx *c, uint8_t *buf, size_t len)
{
    size_t idx = 0;

    while (c->nbuf > 0 && idx < len) {
        uint8_t b = buf[idx];
        uint8_t p = (uint8_t)(b ^ ((c->sbuf >> (32 - c->nbuf)) & 0xFF));
        buf[idx] = p;
        c->mbuf ^= ((uint32_t)p) << (32 - c->nbuf);
        idx++;
        c->nbuf -= 8;
        if (c->nbuf == 0) {
            shannon_macfunc(c, c->mbuf);
        }
    }

    while (idx + 4 <= len) {
        shannon_cycle(c);
        uint32_t word = (uint32_t)buf[idx] |
                        ((uint32_t)buf[idx + 1] << 8) |
                        ((uint32_t)buf[idx + 2] << 16) |
                        ((uint32_t)buf[idx + 3] << 24);
        word ^= c->sbuf;
        shannon_macfunc(c, word);
        buf[idx]     = (uint8_t)word;
        buf[idx + 1] = (uint8_t)(word >> 8);
        buf[idx + 2] = (uint8_t)(word >> 16);
        buf[idx + 3] = (uint8_t)(word >> 24);
        idx += 4;
    }

    if (idx < len) {
        shannon_cycle(c);
        c->mbuf = 0;
        c->nbuf = 32;
        while (idx < len) {
            uint8_t b = buf[idx];
            uint8_t p = (uint8_t)(b ^ ((c->sbuf >> (32 - c->nbuf)) & 0xFF));
            buf[idx] = p;
            c->mbuf ^= ((uint32_t)p) << (32 - c->nbuf);
            idx++;
            c->nbuf -= 8;
        }
    }
}

void shannon_finish(shannon_ctx *c, uint8_t *mac, size_t maclen)
{
    if (c->nbuf != 0) {
        shannon_macfunc(c, c->mbuf);
    }
    shannon_cycle(c);
    c->R[SHANNON_KEYP] ^= SHANNON_INITKONST ^ ((uint32_t)c->nbuf << 3);
    c->nbuf = 0;
    for (int i = 0; i < 16; ++i) {
        c->R[i] ^= c->CRC[i];
    }
    shannon_diffuse(c);

    size_t idx = 0;
    while (idx < maclen) {
        shannon_cycle(c);
        size_t chunk = maclen - idx;
        if (chunk >= 4) {
            mac[idx]     = (uint8_t)c->sbuf;
            mac[idx + 1] = (uint8_t)(c->sbuf >> 8);
            mac[idx + 2] = (uint8_t)(c->sbuf >> 16);
            mac[idx + 3] = (uint8_t)(c->sbuf >> 24);
            idx += 4;
        } else {
            for (size_t i = 0; i < chunk; ++i) {
                mac[idx + i] = (uint8_t)((c->sbuf >> (8 * i)) & 0xFF);
            }
            idx += chunk;
        }
    }
}
