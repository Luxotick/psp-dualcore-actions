#ifndef SPOTIFY_HTTP_H
#define SPOTIFY_HTTP_H

#include <stdint.h>
#include <stddef.h>

/* Thin wrapper over the firmware sceHttp/sceSsl stack (HTTP + HTTPS,
 * redirects handled by firmware). Requires network_probe() to have run. */

typedef struct {
    int tmpl;
    int conn;
    int req;
} http_stream;

/* Loads PARSEURI/PARSEHTTP/HTTP/SSL net modules and initialises the stack. */
int http_init(void);
void http_term(void);

/* Opens a GET request. Returns HTTP status code (>0) or a negative error.
 * *content_length is 0 when the server sent none. On success the caller
 * must http_stream_close() even for non-200 statuses. */
int http_stream_open(http_stream *s, const char *url, uint64_t *content_length);
int http_stream_read(http_stream *s, void *buf, unsigned int len);
void http_stream_close(http_stream *s);

#endif /* SPOTIFY_HTTP_H */
