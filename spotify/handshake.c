#include "handshake.h"
#include "log.h"
#include "dh.h"
#include "sha1.h"
#include <string.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspdebug.h>
#include <pspthreadman.h>

#define SPOTIFY_PORT 443
#define CLIENT_VERSION 124200290ULL
#define PSP_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))
#include "proto_util.h"

static int net_send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int ret = sceNetInetSend(sock, (const char *)(data + sent), (int)(len - sent), 0);
        if (ret <= 0) return -1;
        sent += (size_t)ret;
    }
    return 0;
}

static int net_recv_all(int sock, uint8_t *data, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int ret = sceNetInetRecv(sock, (char *)(data + received), (int)(len - received), 0);
        if (ret <= 0) return -1;
        received += (size_t)ret;
    }
    return 0;
}

static const uint8_t *find_gs_key(const uint8_t *data, size_t len)
{
    /* Locate 96-byte Diffie-Hellman public key in APResponseMessage protobuf */
    size_t idx = 0;
    while (idx < len) {
        uint64_t tag = 0;
        unsigned int shift = 0;
        while (idx < len) {
            uint8_t b = data[idx++];
            tag |= ((uint64_t)(b & 0x7F)) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        uint32_t wire = (uint32_t)(tag & 7);
        if (wire == 0) {
            while (idx < len) {
                if (!(data[idx++] & 0x80)) {
                    break;
                }
            }
        } else if (wire == 2) {
            uint64_t field_len = 0;
            shift = 0;
            while (idx < len) {
                uint8_t b = data[idx++];
                field_len |= ((uint64_t)(b & 0x7F)) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            if (field_len == SPOTIFY_DH_KEY_SIZE && idx + field_len <= len) {
                return &data[idx];
            }
            /* Submessages have wire 2, so keep searching inside if possible */
            if (field_len > SPOTIFY_DH_KEY_SIZE && idx + field_len <= len) {
                const uint8_t *nested = find_gs_key(&data[idx], (size_t)field_len);
                if (nested) return nested;
            }
            idx += (size_t)field_len;
        } else if (wire == 1) {
            idx += 8;
        } else if (wire == 5) {
            idx += 4;
        } else {
            break;
        }
    }
    return NULL;
}

/* Try to resolve a single AP hostname. Returns 0 on success. */
static int resolve_host(const char *host, struct in_addr *out_addr)
{
    char res_buf[1024];
    int rid = -1;
    int ret = sceNetResolverCreate(&rid, res_buf, sizeof(res_buf));
    if (ret < 0) return ret;
    ret = sceNetResolverStartNtoA(rid, host, out_addr, 5, 3);
    sceNetResolverDelete(rid);
    return ret;
}

/* Try to connect to an AP. Returns socket fd >= 0 on success, < 0 on failure.
 * port defaults to SPOTIFY_PORT (443). */
int spotify_connect_and_handshake(spotify_session *session)
{
    memset(session, 0, sizeof(*session));
    session->socket_fd = -1;

    struct in_addr ap_addr;
    memset(&ap_addr, 0, sizeof(ap_addr));
    int ret;

    /* Try multiple modern AP host:port combinations */
    typedef struct { const char *host; int port; } ap_candidate;
    static const ap_candidate candidates[] = {
        {"ap-gew4.spotify.com", 4070},
        {"ap-guc3.spotify.com", 4070},
        {"ap-gew1.spotify.com", 4070},
        {"ap-gae2.spotify.com", 4070},
        {"ap-gew4.spotify.com", 443},
        {"ap-gew4.spotify.com", 80},
        {"ap.spotify.com", SPOTIFY_PORT},  /* old fallback */
    };
    static const int num_candidates = sizeof(candidates) / sizeof(candidates[0]);

    int sock = -1;
    for (int ci = 0; ci < num_candidates; ++ci) {
        const ap_candidate *c = &candidates[ci];

        log_printf("Trying AP %s:%d... ", c->host, c->port);
        ret = resolve_host(c->host, &ap_addr);
        if (ret < 0) {
            log_printf("resolve failed (0x%08X)\n", (unsigned int)ret);
            continue;
        }
        const uint8_t *ip = (const uint8_t *)&ap_addr;
        log_printf("%u.%u.%u.%u ", (unsigned int)ip[0], (unsigned int)ip[1],
                   (unsigned int)ip[2], (unsigned int)ip[3]);

        sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            log_printf("socket failed: %d\n", sock);
            continue;
        }

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = PSP_HTONS(c->port);
        sin.sin_addr = ap_addr;

        ret = sceNetInetConnect(sock, (struct sockaddr *)&sin, sizeof(sin));
        if (ret < 0) {
            log_printf("connect failed: %d\n", ret);
            sceNetInetClose(sock);
            sock = -1;
            continue;
        }
        log_printf("OK\n");
        break;
    }

    if (sock < 0) {
        log_printf("All APs failed\n");
        return -99;
    }
    log_printf("TCP connected to Spotify AP!\n");

    /* Step 1: Generate DH keys */
    uint8_t client_pub[SPOTIFY_DH_KEY_SIZE];
    uint8_t client_priv[SPOTIFY_DH_KEY_SIZE];
    spotify_dh_generate_keys(client_pub, client_priv);

    uint8_t client_nonce[16];
    for (int i = 0; i < 16; ++i) {
        client_nonce[i] = (uint8_t)(sceKernelGetSystemTimeLow() ^ (i * 37));
    }

    /* Step 2: Build ClientHello protobuf */
    uint8_t binfo_buf[32];
    buf_writer binfo_w = { binfo_buf, sizeof(binfo_buf), 0 };
    bw_put_varint_field(&binfo_w, 0x0a, 0); /* PRODUCT_CLIENT */
    bw_put_varint_field(&binfo_w, 0x14, 0); /* PRODUCT_FLAG_NONE */
    bw_put_varint_field(&binfo_w, 0x1e, 2); /* PLATFORM_LINUX_X86 */
    bw_put_varint_field(&binfo_w, 0x28, CLIENT_VERSION);

    uint8_t dh_buf[128];
    buf_writer dh_w = { dh_buf, sizeof(dh_buf), 0 };
    bw_put_bytes(&dh_w, 0x0a, client_pub, SPOTIFY_DH_KEY_SIZE);
    bw_put_varint_field(&dh_w, 0x14, 1); /* server_keys_known = 1 */

    uint8_t login_buf[140];
    buf_writer login_w = { login_buf, sizeof(login_buf), 0 };
    bw_put_bytes(&login_w, 0x0a, dh_buf, dh_w.len);

    uint8_t ch_payload[512];
    buf_writer pw = { ch_payload, sizeof(ch_payload), 0 };
    bw_put_bytes(&pw, 0x0a, binfo_buf, binfo_w.len);
    bw_put_varint_field(&pw, 0x1e, 0); /* CRYPTO_SUITE_SHANNON */
    bw_put_bytes(&pw, 0x32, login_buf, login_w.len);
    bw_put_bytes(&pw, 0x3c, client_nonce, 16);
    uint8_t pad = 0x1e;
    bw_put_bytes(&pw, 0x46, &pad, 1);

    uint8_t ch_packet[550];
    uint32_t ch_size = (uint32_t)(2 + 4 + pw.len);
    ch_packet[0] = 0x00;
    ch_packet[1] = 0x04;
    ch_packet[2] = (uint8_t)(ch_size >> 24);
    ch_packet[3] = (uint8_t)(ch_size >> 16);
    ch_packet[4] = (uint8_t)(ch_size >> 8);
    ch_packet[5] = (uint8_t)(ch_size);
    memcpy(&ch_packet[6], ch_payload, pw.len);

    log_printf("Sending ClientHello (%u bytes)...\n", (unsigned int)ch_size);
    if (net_send_all(sock, ch_packet, ch_size) < 0) {
        log_printf("ClientHello send failed\n");
        sceNetInetClose(sock);
        return -2;
    }

    /* Step 3: Receive APResponseMessage */
    uint8_t resp_hdr[4];
    if (net_recv_all(sock, resp_hdr, 4) < 0) {
        log_printf("APResponseMessage length recv failed\n");
        sceNetInetClose(sock);
        return -3;
    }

    uint32_t resp_size = ((uint32_t)resp_hdr[0] << 24) |
                         ((uint32_t)resp_hdr[1] << 16) |
                         ((uint32_t)resp_hdr[2] << 8) |
                         ((uint32_t)resp_hdr[3]);
    log_printf("APResponse size: %u bytes\n", (unsigned int)resp_size);

    if (resp_size < 4 || resp_size > 4096) {
        log_printf("Invalid APResponse size\n");
        sceNetInetClose(sock);
        return -4;
    }

    uint8_t resp_payload[4096];
    uint32_t payload_len = resp_size - 4;
    if (net_recv_all(sock, resp_payload, payload_len) < 0) {
        log_printf("APResponse payload recv failed\n");
        sceNetInetClose(sock);
        return -5;
    }

    const uint8_t *gs = find_gs_key(resp_payload, payload_len);
    if (!gs) {
        log_printf("Server DH public key (gs) not found in response!\n");
        sceNetInetClose(sock);
        return -6;
    }
    log_printf("Found server DH public key (gs)!\n");

    /* Step 4: Compute shared secret */
    uint8_t shared_secret[SPOTIFY_DH_KEY_SIZE];
    spotify_dh_compute_shared_secret(gs, client_priv, shared_secret);
    log_printf("Computed DH shared secret!\n");

    /* Step 5: Derive keys using HMAC-SHA1 over packet accumulator */
    size_t acc_len = ch_size + resp_size;
    uint8_t acc[5000];
    memcpy(acc, ch_packet, ch_size);
    memcpy(acc + ch_size, resp_hdr, 4);
    memcpy(acc + ch_size + 4, resp_payload, payload_len);

    uint8_t hashed_key[20];
    sha1_ctx key_ctx;
    sha1_init(&key_ctx);
    sha1_update(&key_ctx, shared_secret, SPOTIFY_DH_KEY_SIZE);
    sha1_final(&key_ctx, hashed_key);

    uint8_t ipad[64], opad[64];
    memset(ipad, 0x36, 64);
    memset(opad, 0x5C, 64);
    for (int b = 0; b < 20; ++b) {
        ipad[b] ^= hashed_key[b];
        opad[b] ^= hashed_key[b];
    }

    uint8_t k_data[100];
    for (int i = 1; i <= 5; ++i) {
        uint8_t suffix = (uint8_t)i;
        sha1_ctx inner, outer;

        sha1_init(&inner);
        sha1_update(&inner, ipad, 64);
        sha1_update(&inner, acc, acc_len);
        sha1_update(&inner, &suffix, 1);
        uint8_t inner_hash[20];
        sha1_final(&inner, inner_hash);

        sha1_init(&outer);
        sha1_update(&outer, opad, 64);
        sha1_update(&outer, inner_hash, 20);
        sha1_final(&outer, &k_data[(i - 1) * 20]);
    }

    uint8_t challenge[20];
    hmac_sha1(&k_data[0], 20, acc, acc_len, challenge);

    uint8_t send_key[32];
    uint8_t recv_key[32];
    memcpy(send_key, &k_data[20], 32);
    memcpy(recv_key, &k_data[52], 32);
    log_printf("Derived session keys & challenge!\n");

    /* Step 6: Send ClientResponsePlaintext */
    uint8_t dhr_buf[32];
    buf_writer dhr_w = { dhr_buf, sizeof(dhr_buf), 0 };
    bw_put_bytes(&dhr_w, 0x0a, challenge, 20);

    uint8_t lc_buf[40];
    buf_writer lc_w = { lc_buf, sizeof(lc_buf), 0 };
    bw_put_bytes(&lc_w, 0x0a, dhr_buf, dhr_w.len);

    uint8_t cr_payload[128];
    buf_writer cr_w = { cr_payload, sizeof(cr_payload), 0 };
    bw_put_bytes(&cr_w, 0x0a, lc_buf, lc_w.len);
    bw_put_bytes(&cr_w, 0x14, NULL, 0); /* pow_response */
    bw_put_bytes(&cr_w, 0x1e, NULL, 0); /* crypto_response */

    uint32_t cr_size = (uint32_t)(4 + cr_w.len);
    uint8_t cr_packet[132];
    cr_packet[0] = (uint8_t)(cr_size >> 24);
    cr_packet[1] = (uint8_t)(cr_size >> 16);
    cr_packet[2] = (uint8_t)(cr_size >> 8);
    cr_packet[3] = (uint8_t)(cr_size);
    memcpy(&cr_packet[4], cr_payload, cr_w.len);

    log_printf("Sending ClientResponsePlaintext (%u bytes)...\n", (unsigned int)cr_size);
    if (net_send_all(sock, cr_packet, cr_size) < 0) {
        log_printf("ClientResponsePlaintext send failed\n");
        sceNetInetClose(sock);
        return -7;
    }

    /* Step 7: Initialize Shannon session */
    shannon_init(&session->send_cipher, send_key, 32);
    shannon_init(&session->recv_cipher, recv_key, 32);
    session->send_nonce = 0;
    session->recv_nonce = 0;
    session->socket_fd = sock;
    session->is_connected = 1;

    log_printf("SHANNON HANDSHAKE SUCCESS! Session established!\n");
    return 0;
}

int spotify_send_packet(spotify_session *session, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (!session->is_connected) return -1;

    uint8_t frame[4096];
    if ((size_t)len + 7 > sizeof(frame)) return -2;

    frame[0] = cmd;
    frame[1] = (uint8_t)(len >> 8);
    frame[2] = (uint8_t)(len);
    if (len > 0 && payload) {
        memcpy(&frame[3], payload, len);
    }

    shannon_nonce_u32(&session->send_cipher, session->send_nonce++);
    shannon_encrypt(&session->send_cipher, frame, 3 + len);

    uint8_t mac[4];
    shannon_finish(&session->send_cipher, mac, 4);
    memcpy(&frame[3 + len], mac, 4);

    return net_send_all(session->socket_fd, frame, 3 + len + 4);
}

int spotify_recv_packet(spotify_session *session, uint8_t *cmd, uint8_t *payload_buf, uint16_t max_len, uint16_t *out_len)
{
    if (!session->is_connected) return -1;

    uint8_t header[3];
    if (net_recv_all(session->socket_fd, header, 3) < 0) return -2;

    shannon_nonce_u32(&session->recv_cipher, session->recv_nonce++);
    shannon_decrypt(&session->recv_cipher, header, 3);

    *cmd = header[0];
    uint16_t pkt_len = ((uint16_t)header[1] << 8) | header[2];

    uint16_t to_copy = (pkt_len < max_len) ? pkt_len : max_len;
    uint16_t remaining = pkt_len;

    if (to_copy > 0 && payload_buf) {
        if (net_recv_all(session->socket_fd, payload_buf, to_copy) < 0) return -4;
        shannon_decrypt(&session->recv_cipher, payload_buf, to_copy);
        remaining -= to_copy;
    }

    if (remaining > 0) {
        uint8_t discard[256];
        while (remaining > 0) {
            uint16_t chunk = (remaining > (uint16_t)sizeof(discard)) ? (uint16_t)sizeof(discard) : remaining;
            if (net_recv_all(session->socket_fd, discard, chunk) < 0) return -4;
            shannon_decrypt(&session->recv_cipher, discard, chunk);
            remaining -= chunk;
        }
    }

    uint8_t received_mac[4];
    if (net_recv_all(session->socket_fd, received_mac, 4) < 0) return -5;

    uint8_t computed_mac[4];
    shannon_finish(&session->recv_cipher, computed_mac, 4);

    if (memcmp(received_mac, computed_mac, 4) != 0) {
        return -6; /* MAC verification failed! */
    }

    if (out_len) *out_len = to_copy;
    return 0;
}

void spotify_disconnect(spotify_session *session)
{
    if (session->socket_fd >= 0) {
        sceNetInetClose(session->socket_fd);
        session->socket_fd = -1;
    }
    session->is_connected = 0;
}
