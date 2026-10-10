#include "login.h"
#include "log.h"
#include "proto_util.h"
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>

#define PACKET_TYPE_PING        0x04
#define PACKET_TYPE_PONG        0x49
#define PACKET_TYPE_LOGIN       0xab
#define PACKET_TYPE_WELCOME     0xac
#define PACKET_TYPE_LEGACY_WELCOME 0x69
#define PACKET_TYPE_LOGIN_FAIL  0xad

static void parse_ap_welcome(const uint8_t *data, size_t len,
                             char *canonical_username, size_t max_uname,
                             uint8_t *blob, size_t *blob_len, size_t max_blob)
{
    size_t i = 0;
    while (i < len) {
        uint64_t tag = 0;
        int shift = 0;
        while (i < len) {
            uint8_t b = data[i++];
            tag |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        uint32_t field_num = (uint32_t)(tag >> 3);
        uint32_t wire_type = (uint32_t)(tag & 7);

        if (wire_type == 0) {
            while (i < len && (data[i++] & 0x80)) {}
        } else if (wire_type == 2) {
            uint64_t field_len = 0;
            shift = 0;
            while (i < len) {
                uint8_t b = data[i++];
                field_len |= (uint64_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            if (i + field_len > len) break;

            /* Matches canonical_username (field 1 or 0x0a) */
            if (field_num == 1 || field_num == 0x0a) {
                size_t copy_len = field_len < max_uname - 1 ? (size_t)field_len : max_uname - 1;
                memcpy(canonical_username, &data[i], copy_len);
                canonical_username[copy_len] = '\0';
            }
            /* Matches reusable_auth_credentials (field 5, 0x28, or 0x05) */
            else if (field_num == 5 || field_num == 0x28 || field_num == (0x28 >> 3)) {
                size_t copy_len = field_len < max_blob ? (size_t)field_len : max_blob;
                memcpy(blob, &data[i], copy_len);
                *blob_len = copy_len;
            }

            i += (size_t)field_len;
        } else {
            break;
        }
    }
}

int spotify_login(spotify_session *session, spotify_config *cfg)
{
    if (!session || !session->is_connected) {
        log_printf("LOGIN: Session not connected!\n");
        return -1;
    }

    log_printf("LOGIN: Preparing authentication packet (type %d)...\n", cfg->auth_type);

    /* 1. LoginCredentials */
    uint8_t cred_buf[1536];
    buf_writer cred_w = { cred_buf, sizeof(cred_buf), 0 };

    if (cfg->username[0]) {
        bw_put_string(&cred_w, 0x0a, cfg->username);
    }
    bw_put_varint_field(&cred_w, 0x14, (uint64_t)cfg->auth_type);

    if (cfg->auth_type == AUTH_TYPE_STORED_CREDENTIALS && cfg->blob_len > 0) {
        bw_put_bytes(&cred_w, 0x1e, cfg->blob, cfg->blob_len);
    } else if (cfg->auth_type == AUTH_TYPE_SPOTIFY_TOKEN && cfg->token[0]) {
        log_printf("LOGIN: Token len = %u bytes\n", (unsigned int)strlen(cfg->token));
        bw_put_bytes(&cred_w, 0x1e, (const uint8_t *)cfg->token, strlen(cfg->token));
    } else if (cfg->auth_type == AUTH_TYPE_USER_PASS && cfg->password[0]) {
        bw_put_bytes(&cred_w, 0x1e, (const uint8_t *)cfg->password, strlen(cfg->password));
    }

    /* 2. SystemInfo */
    uint8_t sys_buf[256];
    buf_writer sys_w = { sys_buf, sizeof(sys_buf), 0 };
    bw_put_varint_field(&sys_w, 0x0a, 0); /* CPU_UNKNOWN */
    bw_put_varint_field(&sys_w, 0x3c, 2); /* OS_LINUX */
    bw_put_string(&sys_w, 0x5a, "Sony PlayStation Portable (PSP-3000)");
    bw_put_string(&sys_w, 0x64, SPOTIFY_DEVICE_ID);

    /* 3. ClientResponseEncrypted */
    uint8_t cre_buf[2048];
    buf_writer cre_w = { cre_buf, sizeof(cre_buf), 0 };
    bw_put_bytes(&cre_w, 0x0a, cred_buf, cred_w.len);
    bw_put_bytes(&cre_w, 0x32, sys_buf, sys_w.len);
    bw_put_string(&cre_w, 0x46, "librespot-c_master_dev");

    log_printf("LOGIN: Sending Login packet 0x%02X (%u bytes)...\n",
                         PACKET_TYPE_LOGIN, (unsigned int)cre_w.len);

    int ret = spotify_send_packet(session, PACKET_TYPE_LOGIN, cre_buf, (uint16_t)cre_w.len);
    if (ret < 0) {
        log_printf("LOGIN: Failed to send packet (%d)\n", ret);
        return ret;
    }

    /* 4. Await APWelcome (0xAC) or APLoginFailed (0xAD) */
    log_printf("LOGIN: Waiting for Spotify response...\n");

    uint8_t rx_buf[4096];
    for (int attempts = 0; attempts < 10; ++attempts) {
        uint8_t cmd = 0;
        uint16_t rx_len = 0;
        int ret_pkt = spotify_recv_packet(session, &cmd, rx_buf, sizeof(rx_buf), &rx_len);
        if (ret_pkt < 0) {
            log_printf("LOGIN: Error reading response (%d)\n", ret_pkt);
            return ret_pkt;
        }

        if (cmd == PACKET_TYPE_WELCOME || cmd == PACKET_TYPE_LEGACY_WELCOME) {
            char canonical_uname[128] = {0};
            uint8_t new_blob[SPOTIFY_CFG_MAX_BLOB] = {0};
            size_t new_blob_len = 0;

            parse_ap_welcome(rx_buf, (size_t)rx_len, canonical_uname, sizeof(canonical_uname),
                             new_blob, &new_blob_len, sizeof(new_blob));

            pspDebugScreenClear();
            log_printf("========================================\n");
            log_printf("SPOTIFY LOGIN SUCCESSFUL!\n");
            if (canonical_uname[0]) {
                log_printf("User: %s\n", canonical_uname);
            }
            log_printf("========================================\n");

            /* If Spotify gave us reusable credentials, save them to Memory Stick */
            if (new_blob_len > 0) {
                spotify_config_save_blob(cfg, canonical_uname, new_blob, new_blob_len);
            }
            return 0;
        } else if (cmd == PACKET_TYPE_LOGIN_FAIL) {
            log_printf("LOGIN FAILED (0xAD)! Payload %u bytes:\n", (unsigned int)rx_len);
            for (size_t k = 0; k < rx_len && k < 16; ++k) {
                log_printf("%02X ", rx_buf[k]);
            }
            log_printf("\n");
            return -2;
        } else if (cmd == PACKET_TYPE_PING) {
            spotify_send_packet(session, PACKET_TYPE_PONG, rx_buf, rx_len);
        } else {
            log_printf("LOGIN: Received packet 0x%02X (%u bytes), continuing wait...\n", cmd, (unsigned int)rx_len);
        }
    }

    log_printf("LOGIN: Timed out waiting for APWelcome\n");
    return -3;
}
