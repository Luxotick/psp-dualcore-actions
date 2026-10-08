#include "audiokey.h"
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>

#define PACKET_TYPE_REQUEST_KEY 0x0c
#define PACKET_TYPE_AES_KEY     0x0d
#define PACKET_TYPE_AES_KEY_ERR 0x0e
#define PACKET_TYPE_PING        0x04
#define PACKET_TYPE_PONG        0x49

static uint32_t key_seq = 1;

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
        } else {
            pspDebugScreenPrintf("AUDIOKEY: Ignoring packet 0x%02X (%u bytes)\n", cmd, (unsigned int)rx_len);
        }
    }

    pspDebugScreenPrintf("AUDIOKEY: Timeout waiting for AesKey\n");
    return -4;
}
