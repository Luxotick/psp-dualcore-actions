#ifndef SPOTIFY_TLS_H
#define SPOTIFY_TLS_H

#include <stddef.h>

/* TLS 1.2 client on top of a connected PSP TCP socket, using the bundled
 * BearSSL. The firmware sceSsl stack cannot negotiate TLS 1.2. */

typedef struct tls_conn tls_conn;

/* Gathers timing-jitter entropy once; call after the network is up. */
void tls_seed_entropy(void);

/* Wraps sock (already connected) and runs the handshake for host.
 * Returns NULL on failure (error already logged). The socket is not closed. */
tls_conn *tls_open(int sock, const char *host);
int tls_write_all(tls_conn *c, const void *buf, size_t len);
int tls_read(tls_conn *c, void *buf, size_t len);
/* Sends close_notify (best effort) and frees the context. */
void tls_close(tls_conn *c);

#endif /* SPOTIFY_TLS_H */
