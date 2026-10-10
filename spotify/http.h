#ifndef SPOTIFY_HTTP_H
#define SPOTIFY_HTTP_H

#include <stdint.h>
#include <stddef.h>

/* Minimal HTTP/1.1 client over PSP sockets, with TLS 1.2 via BearSSL for
 * https:// URLs. Follows up to 5 redirects; decodes chunked bodies. */

struct tls_conn;

typedef struct {
    int sock;
    struct tls_conn *tls;
    /* Body framing */
    int chunked;
    int chunk_started;
    uint64_t remaining;      /* bytes left in body (or current chunk) */
    int body_done;
    /* Bytes already pulled from the transport but not yet returned */
    unsigned char buf[2048];
    size_t buf_pos, buf_len;
} http_stream;

/* Seeds TLS entropy. Call once after the network is up. */
int http_init(void);
void http_term(void);

/* Opens a request. method is "GET" or "POST"; extra_headers is NULL or
 * CRLF-terminated header lines. Returns the HTTP status (>0) or a negative
 * error. *content_length is 0 when unknown (chunked or absent). After a
 * positive return the caller must http_stream_close(). */
int http_stream_open_ex(http_stream *s, const char *method, const char *url,
                        const char *extra_headers, const void *body, size_t body_len,
                        uint64_t *content_length);
int http_stream_open(http_stream *s, const char *url, uint64_t *content_length);
/* Returns bytes read, 0 at end of body, or < 0 on error. */
int http_stream_read(http_stream *s, void *buf, unsigned int len);
void http_stream_close(http_stream *s);

#endif /* SPOTIFY_HTTP_H */
