#ifndef PSP_SPOTIFY_HANDSHAKE_H
#define PSP_SPOTIFY_HANDSHAKE_H

#include <stdint.h>
#include <stddef.h>
#include "shannon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int socket_fd;
    shannon_ctx send_cipher;
    shannon_ctx recv_cipher;
    uint32_t send_nonce;
    uint32_t recv_nonce;
    int is_connected;
} spotify_session;

/**
 * Connect to Spotify Access Point (ap.spotify.com:443) and complete
 * native Diffie-Hellman + Shannon cryptographic handshake.
 * Returns 0 on success, negative error code on failure.
 */
int spotify_connect_and_handshake(spotify_session *session);

/**
 * Send an encrypted, MAC-protected packet to Spotify AP.
 * Packet structure: cmd (1B) + length (2B BE) + payload + Shannon MAC (4B).
 */
int spotify_send_packet(spotify_session *session, uint8_t cmd, const uint8_t *payload, uint16_t len);

/**
 * Receive and decrypt a MAC-verified packet from Spotify AP.
 * Returns 0 on success, negative error code on failure.
 */
int spotify_recv_packet(spotify_session *session, uint8_t *cmd, uint8_t *payload_buf, uint16_t max_len, uint16_t *out_len);

/**
 * 1 if a packet (or EOF) is waiting, 0 if not within timeout_ms, < 0 on error.
 */
int spotify_poll_readable(spotify_session *session, int timeout_ms);

/**
 * Close socket and release session.
 */
void spotify_disconnect(spotify_session *session);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SPOTIFY_HANDSHAKE_H */
