#include "dh.h"
#include <string.h>
#include <pspthreadman.h>

/* Oakley Group 1 768-bit MODP prime (RFC 2409 / Spotify) */
static const uint8_t DH_PRIME[SPOTIFY_DH_KEY_SIZE] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
    0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
    0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
    0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
    0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

typedef struct {
    uint32_t d[24];
} bn768;

static void bn_from_bytes_be(bn768 *r, const uint8_t *b)
{
    for (int i = 0; i < 24; ++i) {
        int idx = (23 - i) * 4;
        r->d[i] = ((uint32_t)b[idx] << 24) |
                  ((uint32_t)b[idx + 1] << 16) |
                  ((uint32_t)b[idx + 2] << 8) |
                  ((uint32_t)b[idx + 3]);
    }
}

static void bn_to_bytes_be(uint8_t *b, const bn768 *r)
{
    for (int i = 0; i < 24; ++i) {
        int idx = (23 - i) * 4;
        b[idx]     = (uint8_t)(r->d[i] >> 24);
        b[idx + 1] = (uint8_t)(r->d[i] >> 16);
        b[idx + 2] = (uint8_t)(r->d[i] >> 8);
        b[idx + 3] = (uint8_t)(r->d[i]);
    }
}

static int bn_cmp(const bn768 *a, const bn768 *b)
{
    for (int i = 23; i >= 0; --i) {
        if (a->d[i] > b->d[i]) return 1;
        if (a->d[i] < b->d[i]) return -1;
    }
    return 0;
}

static void bn_sub(bn768 *r, const bn768 *a, const bn768 *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < 24; ++i) {
        borrow += (int64_t)a->d[i] - b->d[i];
        r->d[i] = (uint32_t)borrow;
        borrow >>= 32;
    }
}

static void bn_mul48(uint32_t prod[48], const bn768 *a, const bn768 *b)
{
    memset(prod, 0, 48 * sizeof(uint32_t));
    for (int i = 0; i < 24; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < 24; ++j) {
            uint64_t sum = (uint64_t)a->d[i] * b->d[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)sum;
            carry = sum >> 32;
        }
        prod[i + 24] += (uint32_t)carry;
    }
}

static void bn_mod_p(bn768 *r, uint32_t u[49], const bn768 *p)
{
    for (int j = 47; j >= 24; --j) {
        while (u[j] > 0 || (u[j] == 0 && u[j - 1] >= p->d[23])) {
            uint64_t num = ((uint64_t)u[j] << 32) | u[j - 1];
            uint64_t q = num / (uint64_t)p->d[23];
            if (q > 0xFFFFFFFFULL) q = 0xFFFFFFFFULL;
            if (q == 0) q = 1;

            int64_t borrow = 0;
            for (int i = 0; i < 24; ++i) {
                uint64_t mul = q * (uint64_t)p->d[i];
                borrow += (int64_t)u[j - 24 + i] - (int64_t)(uint32_t)mul;
                u[j - 24 + i] = (uint32_t)borrow;
                borrow >>= 32;
                borrow -= (int64_t)(mul >> 32);
            }
            borrow += (int64_t)u[j];
            u[j] = (uint32_t)borrow;
            borrow >>= 32;

            while (borrow < 0) {
                uint64_t carry = 0;
                for (int i = 0; i < 24; ++i) {
                    carry += (uint64_t)u[j - 24 + i] + p->d[i];
                    u[j - 24 + i] = (uint32_t)carry;
                    carry >>= 32;
                }
                carry += (uint64_t)u[j];
                u[j] = (uint32_t)carry;
                borrow += (int64_t)(carry >> 32);
            }
        }
    }

    for (int i = 0; i < 24; ++i) {
        r->d[i] = u[i];
    }
    while (bn_cmp(r, p) >= 0) {
        bn_sub(r, r, p);
    }
}

static void bn_modpow(bn768 *r, const bn768 *base, const bn768 *exp, const bn768 *p)
{
    bn768 res;
    memset(&res, 0, sizeof(res));
    res.d[0] = 1;

    bn768 cur = *base;
    uint32_t prod[49];

    for (int i = 0; i < 24; ++i) {
        uint32_t w = exp->d[i];
        for (int bit = 0; bit < 32; ++bit) {
            if ((w & (1u << bit)) != 0) {
                bn_mul48(prod, &res, &cur);
                prod[48] = 0;
                bn_mod_p(&res, prod, p);
            }
            bn_mul48(prod, &cur, &cur);
            prod[48] = 0;
            bn_mod_p(&cur, prod, p);
        }
    }
    *r = res;
}

static void fill_random(uint8_t *buf, size_t len)
{
    static uint32_t seed = 0;
    if (seed == 0) {
        seed = sceKernelGetSystemTimeLow() ^ 0x5DEECE66DULL;
    }
    for (size_t i = 0; i < len; ++i) {
        seed = seed * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(seed >> 16);
    }
}

void spotify_dh_generate_keys(uint8_t public_key[SPOTIFY_DH_KEY_SIZE],
                              uint8_t private_key[SPOTIFY_DH_KEY_SIZE])
{
    bn768 p, g, priv, pub;
    bn_from_bytes_be(&p, DH_PRIME);

    memset(&g, 0, sizeof(g));
    g.d[0] = 2; /* Generator g = 2 */

    fill_random(private_key, SPOTIFY_DH_KEY_SIZE);
    private_key[0] = 0; /* Ensure private key < P */
    if (private_key[SPOTIFY_DH_KEY_SIZE - 1] == 0) {
        private_key[SPOTIFY_DH_KEY_SIZE - 1] = 0x42;
    }

    bn_from_bytes_be(&priv, private_key);
    bn_modpow(&pub, &g, &priv, &p);
    bn_to_bytes_be(public_key, &pub);
}

void spotify_dh_compute_shared_secret(const uint8_t remote_public_key[SPOTIFY_DH_KEY_SIZE],
                                      const uint8_t private_key[SPOTIFY_DH_KEY_SIZE],
                                      uint8_t shared_secret[SPOTIFY_DH_KEY_SIZE])
{
    bn768 p, remote_pub, priv, secret;
    bn_from_bytes_be(&p, DH_PRIME);
    bn_from_bytes_be(&remote_pub, remote_public_key);
    bn_from_bytes_be(&priv, private_key);

    bn_modpow(&secret, &remote_pub, &priv, &p);
    bn_to_bytes_be(shared_secret, &secret);
}

int spotify_dh_selftest(void)
{
    uint8_t alice_pub[SPOTIFY_DH_KEY_SIZE];
    uint8_t alice_priv[SPOTIFY_DH_KEY_SIZE];
    uint8_t bob_pub[SPOTIFY_DH_KEY_SIZE];
    uint8_t bob_priv[SPOTIFY_DH_KEY_SIZE];
    uint8_t alice_secret[SPOTIFY_DH_KEY_SIZE];
    uint8_t bob_secret[SPOTIFY_DH_KEY_SIZE];

    spotify_dh_generate_keys(alice_pub, alice_priv);
    spotify_dh_generate_keys(bob_pub, bob_priv);

    spotify_dh_compute_shared_secret(bob_pub, alice_priv, alice_secret);
    spotify_dh_compute_shared_secret(alice_pub, bob_priv, bob_secret);

    if (memcmp(alice_secret, bob_secret, SPOTIFY_DH_KEY_SIZE) != 0) {
        return -1;
    }
    return 0;
}
