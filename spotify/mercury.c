#include "mercury.h"
#include "config.h"
#include "log.h"
#include "proto_util.h"
#include <stdio.h>
#include <string.h>

#define PACKET_PING         0x04
#define PACKET_PONG         0x49
#define PACKET_MERCURY_REQ  0xb2
#define MERCURY_FLAG_FINAL  0x01

static uint64_t mercury_seq;
static uint8_t rx_buf[16384];

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int spotify_mercury_get(spotify_session *session, const char *uri,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    uint8_t header[512];
    buf_writer hw = { header, sizeof header, 0 };
    bw_put_string(&hw, 1, uri);
    bw_put_string(&hw, 3, "GET");
    if (hw.len == hw.cap) return -1;

    /* seq_len(2) seq(8) flags(1) part_count(2) { part_len(2) part }... */
    uint8_t seq[8];
    uint64_t s = ++mercury_seq;
    for (int i = 0; i < 8; ++i) seq[i] = (uint8_t)(s >> (56 - 8 * i));
    uint8_t pkt[600];
    size_t n = 0;
    pkt[n++] = 0; pkt[n++] = 8;
    memcpy(pkt + n, seq, 8); n += 8;
    pkt[n++] = MERCURY_FLAG_FINAL;
    pkt[n++] = 0; pkt[n++] = 1;
    pkt[n++] = (uint8_t)(hw.len >> 8); pkt[n++] = (uint8_t)hw.len;
    memcpy(pkt + n, header, hw.len); n += hw.len;

    int ret = spotify_send_packet(session, PACKET_MERCURY_REQ, pkt, (uint16_t)n);
    if (ret < 0) return ret;

    for (int attempt = 0; attempt < 40; ++attempt) {
        uint8_t cmd = 0;
        uint16_t len = 0;
        ret = spotify_recv_packet(session, &cmd, rx_buf, sizeof rx_buf, &len);
        if (ret < 0) return ret;
        if (cmd == PACKET_PING) {
            spotify_send_packet(session, PACKET_PONG, rx_buf, len);
            continue;
        }
        if (cmd != PACKET_MERCURY_REQ || len < 13) continue;

        size_t i = 0;
        size_t seq_len = be16(rx_buf);
        i = 2;
        if (seq_len != 8 || i + seq_len + 3 > len || memcmp(rx_buf + i, seq, 8) != 0) continue;
        i += seq_len;
        uint8_t flags = rx_buf[i++];
        size_t count = be16(rx_buf + i);
        i += 2;
        if (flags != MERCURY_FLAG_FINAL) {
            log_printf("MERCURY: multi-packet response not supported (flags %u)\n", flags);
            return -2;
        }

        int status = 0;
        for (size_t part = 0; part < count; ++part) {
            if (i + 2 > len) return -3;
            size_t part_len = be16(rx_buf + i);
            i += 2;
            if (i + part_len > len) return -3;
            if (part == 0) {
                pb_reader r;
                pb_field f;
                pb_init(&r, rx_buf + i, part_len);
                while (pb_next(&r, &f) == 1)
                    if (f.field == 4 && f.wire == 0)   /* sint32 status_code (zigzag) */
                        status = (int)((f.varint >> 1) ^ (~(f.varint & 1) + 1));
            } else if (part == 1) {
                size_t copy = part_len < cap ? part_len : cap;
                memcpy(out, rx_buf + i, copy);
                *out_len = copy;
            }
            i += part_len;
        }
        return status;
    }
    log_printf("MERCURY: no response for %s\n", uri);
    return -4;
}

int spotify_keymaster_token(spotify_session *session, const char *client_id,
                            const char *scopes, char *token, size_t cap)
{
    char uri[256];
    int n = snprintf(uri, sizeof uri,
                     "hm://keymaster/token/authenticated?scope=%s&client_id=%s&device_id=%s",
                     scopes, client_id, SPOTIFY_DEVICE_ID);
    if (n < 0 || (size_t)n >= sizeof uri) return -1;

    static uint8_t json[2048];
    size_t len = 0;
    int status = spotify_mercury_get(session, uri, json, sizeof json - 1, &len);
    json[len] = '\0';
    if (status != 200) {
        /* Error payloads are short JSON/text; show them (no secrets inside). */
        for (size_t i = 0; i < len; ++i)
            if (json[i] < 0x20 || json[i] > 0x7e) json[i] = '.';
        log_printf("KEYMASTER: status %d: %.80s\n", status, (const char *)json);
        return status < 0 ? status : -status;
    }

    const char *key = strstr((const char *)json, "\"accessToken\"");
    if (!key) return -5;
    const char *start = strchr(key + 13, '"');
    if (!start) return -5;
    ++start;
    const char *end = strchr(start, '"');
    if (!end || (size_t)(end - start) + 1 > cap) return -6;
    memcpy(token, start, (size_t)(end - start));
    token[end - start] = '\0';
    log_printf("KEYMASTER: access token ok (%u chars)\n", (unsigned int)(end - start));
    return 0;
}
