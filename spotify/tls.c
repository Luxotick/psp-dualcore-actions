#include "tls.h"
#include "log.h"
#include "trust_anchors.h"
#include <bearssl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pspthreadman.h>
#include <psprtc.h>
#include <pspwlan.h>
#include <pspnet_inet.h>

struct tls_conn {
    int sock;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* No OS RNG on the PSP: BearSSL is built with every system seeder off and
 * gets its seed from this pool. */
static unsigned char entropy_pool[32];
static uint32_t entropy_counter;
static int entropy_ready;

void tls_seed_entropy(void)
{
    br_sha256_context h;
    br_sha256_init(&h);
    if (entropy_ready) br_sha256_update(&h, entropy_pool, sizeof entropy_pool);

    uint8_t mac[8] = {0};
    sceWlanGetEtherAddr(mac);
    br_sha256_update(&h, mac, sizeof mac);
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    br_sha256_update(&h, &tick, sizeof tick);

    /* Thread wake-up latency and spin counts vary with interrupts (Wi-Fi,
     * timers, ME); hash 512 samples of that jitter. */
    for (int i = 0; i < 512; ++i) {
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        sceKernelDelayThread(50 + (i & 15));
        SceInt64 t1 = sceKernelGetSystemTimeWide();
        uint32_t spins = 0;
        while (sceKernelGetSystemTimeWide() == t1 && spins < 100000) ++spins;
        uint32_t sample[3] = { (uint32_t)t0, (uint32_t)(t1 - t0), spins };
        br_sha256_update(&h, sample, sizeof sample);
    }
    br_sha256_out(&h, entropy_pool);
    entropy_ready = 1;
}

static void connection_seed(unsigned char out[32])
{
    if (!entropy_ready) tls_seed_entropy();
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, entropy_pool, sizeof entropy_pool);
    ++entropy_counter;
    br_sha256_update(&h, &entropy_counter, sizeof entropy_counter);
    SceInt64 now = sceKernelGetSystemTimeWide();
    br_sha256_update(&h, &now, sizeof now);
    br_sha256_out(&h, out);
}

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int r = sceNetInetRecv(*(int *)ctx, buf, len, 0);
    return r > 0 ? r : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int w = sceNetInetSend(*(int *)ctx, buf, len, 0);
    return w > 0 ? w : -1;
}

tls_conn *tls_open(int sock, const char *host)
{
    tls_conn *c = malloc(sizeof *c);
    if (!c) {
        log_printf("TLS: out of memory\n");
        return NULL;
    }
    c->sock = sock;

    br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);
    br_ssl_engine_set_versions(&c->sc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);

    /* PSP RTC tick counts microseconds since 0001-01-01 UTC; BearSSL wants
     * days since 0000-01-01 (year 0 is a 366-day leap year). */
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    u64 secs = tick / 1000000u;
    uint32_t days = (uint32_t)(secs / 86400u) + 366u;
    br_x509_minimal_set_time(&c->xc, days, (uint32_t)(secs % 86400u));

    unsigned char seed[32];
    connection_seed(seed);
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);

    if (!br_ssl_client_reset(&c->sc, host, 0)) {
        log_printf("TLS: reset failed (err %d)\n", br_ssl_engine_last_error(&c->sc.eng));
        free(c);
        return NULL;
    }
    br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->sock, sock_write, &c->sock);

    log_printf("TLS: handshake with %s...\n", host);
    /* Drive the handshake now so failures are reported here. */
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    if (br_sslio_flush(&c->io) < 0 ||
        br_ssl_engine_current_state(&c->sc.eng) == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&c->sc.eng);
        log_printf("TLS: handshake with %s failed, BearSSL err %d\n", host, err);
        if (err >= 53 && err <= 54)
            log_printf("TLS: check PSP clock (RTC day %u)\n", (unsigned int)days);
        free(c);
        return NULL;
    }
    log_printf("TLS: %s ok, suite 0x%04X, %u ms\n", host,
               (unsigned int)c->sc.eng.session.cipher_suite,
               (unsigned int)((sceKernelGetSystemTimeWide() - t0) / 1000));
    return c;
}

int tls_write_all(tls_conn *c, const void *buf, size_t len)
{
    if (br_sslio_write_all(&c->io, buf, len) < 0) return -1;
    return br_sslio_flush(&c->io);
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
    int r = br_sslio_read(&c->io, buf, len);
    if (r < 0) {
        /* A clean close_notify reports error 0; anything else is a failure. */
        int err = br_ssl_engine_last_error(&c->sc.eng);
        if (err != BR_ERR_OK) log_printf("TLS: read failed, BearSSL err %d\n", err);
        return err == BR_ERR_OK ? 0 : -1;
    }
    return r;
}

void tls_close(tls_conn *c)
{
    if (!c) return;
    /* Do not wait for the peer's close_notify: servers using
     * Connection: close often just drop the socket. */
    br_ssl_engine_close(&c->sc.eng);
    free(c);
}
