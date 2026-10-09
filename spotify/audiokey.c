#include "audiokey.h"
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>

#define PACKET_TYPE_REQUEST_KEY 0x0c
#define PACKET_TYPE_AES_KEY     0x0d
#define PACKET_TYPE_AES_KEY_ERR 0x0e
#define PACKET_TYPE_PING        0x04
#define PACKET_TYPE_PONG        0x49
#define PACKET_TYPE_UNKNOWN_ZEROS 0x1f
#define PACKET_TYPE_LEGACY_WELCOME 0x69
#define PACKET_TYPE_MERCURY_EVENT 0xb5

static uint32_t key_seq = 1;

/* Minimal MercuryEvent (0xB5) parser.
 * Layout (big-endian):
 *   2B seq_len + seq_len bytes seq
 *   1B flags
 *   2B count
 *   count * (2B part_len + part_len bytes data)
 * Part 0 is always a Mercury header protobuf containing:
 *   field 1 (0x0a): status_code (varint)
 *   field 2 (0x12): uri (length-delimited string)
 */
static void parse_mercury_event(const uint8_t *data, size_t len)
{
    /* Read Mercury envelope: seq_len + seq + flags + count */
    size_t i = 0;
    if (i + 2 > len) return;
    size_t seq_len = ((size_t)data[i] << 8) | data[i + 1];
    i += 2 + seq_len;
    if (i + 3 > len) return;
    uint8_t flags = data[i++];
    size_t count = ((size_t)data[i] << 8) | data[i + 1];
    i += 2;

    /* Read part 0 (header) length + pointer */
    if (count == 0 || i + 2 > len) return;
    size_t part0_len = ((size_t)data[i] << 8) | data[i + 1];
    const uint8_t *hdr = &data[i + 2];
    i += 2 + part0_len; /* i now points past part 0 */
    if ((size_t)(hdr - data) + part0_len > len) return;

    /* Parse protobuf fields from header part (field_num << 3 | wire_type) */
    uint16_t status_code = 0;
    const char *uri = "";
    size_t h = 0;
    while (h < part0_len) {
        uint64_t tag = 0;
        size_t shift = 0;
        while (h < part0_len) {
            tag |= (uint64_t)(hdr[h] & 0x7F) << shift;
            if (!(hdr[h] & 0x80)) { h++; break; }
            h++; shift += 7;
        }
        if (h >= part0_len) break;
        uint32_t field_num = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 7);

        if (wire_type == 0) {
            uint64_t val = 0;
            shift = 0;
            while (h < part0_len) {
                val |= (uint64_t)(hdr[h] & 0x7F) << shift;
                if (!(hdr[h] & 0x80)) { h++; break; }
                h++; shift += 7;
            }
            if (field_num == 1) status_code = (uint16_t)val;
        } else if (wire_type == 2) {
            uint64_t str_len = 0;
            shift = 0;
            while (h < part0_len) {
                str_len |= (uint64_t)(hdr[h] & 0x7F) << shift;
                if (!(hdr[h] & 0x80)) { h++; break; }
                h++; shift += 7;
            }
            if (field_num == 2 && h + str_len <= part0_len)
                uri = (const char *)&hdr[h];
            h += str_len;
        } else {
            break;
        }
    }

    pspDebugScreenPrintf("MERCURY: status=%u uri=%s flags=0x%02X count=%u\n",
                         status_code, uri, flags, (unsigned int)count);
    if (status_code >= 400 && i < len) {
        size_t dump = len - i;
        if (dump > 32) dump = 32;
        pspDebugScreenPrintf("MERCURY ERROR: ");
        for (size_t k = 0; k < dump; ++k)
            pspDebugScreenPrintf("%02X ", data[i + k]);
        pspDebugScreenPrintf("\n");
    }
}

int spotify_request_audio_key(spotify_session *session,
                              const uint8_t *file_id,
                              const uint8_t *track_gid,
                              uint8_t out_aes_key[16])
{
    if (!session || !session->is_connected) {
        pspDebugScreenPrintf("AUDIOKEY: Session not connected!\n");
        return -1;
    }

    uint8_t req[42];
    memcpy(&req[0], file_id, 20);
    memcpy(&req[20], track_gid, 16);

    uint32_t cur_seq = key_seq++;
    req[36] = (uint8_t)(cur_seq >> 24);
    req[37] = (uint8_t)(cur_seq >> 16);
    req[38] = (uint8_t)(cur_seq >> 8);
    req[39] = (uint8_t)(cur_seq);
    req[40] = 0x00;
    req[41] = 0x00;

    pspDebugScreenPrintf("AUDIOKEY: Requesting AES-128 key (seq=%u)...\n", (unsigned int)cur_seq);
    int ret = spotify_send_packet(session, PACKET_TYPE_REQUEST_KEY, req, sizeof(req));
    if (ret < 0) {
        pspDebugScreenPrintf("AUDIOKEY: Failed to send RequestKey packet (%d)\n", ret);
        return ret;
    }

    uint8_t rx_buf[4096];
    for (int attempts = 0; attempts < 30; ++attempts) {
        uint8_t cmd = 0;
        uint16_t rx_len = 0;
        ret = spotify_recv_packet(session, &cmd, rx_buf, sizeof(rx_buf), &rx_len);
        if (ret < 0) {
            pspDebugScreenPrintf("AUDIOKEY: Recv failed (%d)\n", ret);
            return ret;
        }

        if (cmd == PACKET_TYPE_AES_KEY) {
            if (rx_len < 20) {
                pspDebugScreenPrintf("AUDIOKEY: Payload too short (%u)\n", (unsigned int)rx_len);
                return -2;
            }
            uint32_t resp_seq = ((uint32_t)rx_buf[0] << 24) |
                                ((uint32_t)rx_buf[1] << 16) |
                                ((uint32_t)rx_buf[2] << 8) |
                                ((uint32_t)rx_buf[3]);
            if (resp_seq != cur_seq) {
                pspDebugScreenPrintf("AUDIOKEY: Seq mismatch (%u != %u)\n", (unsigned int)resp_seq, (unsigned int)cur_seq);
            }
            memcpy(out_aes_key, &rx_buf[4], 16);

            pspDebugScreenPrintf("AUDIOKEY: SUCCESS! AES Key: ");
            for (int k = 0; k < 16; ++k) {
                pspDebugScreenPrintf("%02X", out_aes_key[k]);
            }
            pspDebugScreenPrintf("\n");
            return 0;
        } else if (cmd == PACKET_TYPE_AES_KEY_ERR) {
            uint16_t err = 0;
            if (rx_len >= 6) {
                err = (uint16_t)(((uint16_t)rx_buf[4] << 8) | rx_buf[5]);
            }
            pspDebugScreenPrintf("AUDIOKEY: Spotify returned error code 0x%04X!\n", err);
            return -3;
        } else if (cmd == PACKET_TYPE_PING) {
            spotify_send_packet(session, PACKET_TYPE_PONG, rx_buf, rx_len);
        } else if (cmd == PACKET_TYPE_MERCURY_EVENT) {
            /* AP Mercury events arrive on the same socket.
             * Parse status/URI to diagnose errors. */
            if (rx_len > 0) {
                parse_mercury_event(rx_buf, (size_t)rx_len);
            }
        } else if (cmd == PACKET_TYPE_UNKNOWN_ZEROS || cmd == PACKET_TYPE_LEGACY_WELCOME) {
            /* Expected AP noise — silently consume. */
        } else {
            pspDebugScreenPrintf("AUDIOKEY: Ignoring packet 0x%02X (%u bytes)\n", cmd, (unsigned int)rx_len);
        }
    }

    pspDebugScreenPrintf("AUDIOKEY: Timeout waiting for AesKey\n");
    return -4;
}
