#include "http.h"
#include "log.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspthreadman.h>

#define PSP_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))
#define BODY_UNTIL_CLOSE UINT64_MAX
#define MAX_REDIRECTS 5
#define SOCK_TIMEOUT_US (15u * 1000u * 1000u)

typedef struct {
    int https;
    char host[256];
    unsigned short port;
    char path[1024];
} url_parts;

int http_init(void)
{
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    tls_seed_entropy();
    log_printf("HTTP: TLS entropy seeded in %u ms\n",
               (unsigned int)((sceKernelGetSystemTimeWide() - t0) / 1000));
    return 0;
}

void http_term(void)
{
}

static int parse_url(const char *url, url_parts *u)
{
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        u->https = 1; u->port = 443; p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        u->https = 0; u->port = 80; p = url + 7;
    } else {
        return -1;
    }
    size_t host_len = strcspn(p, ":/?");
    if (host_len == 0 || host_len >= sizeof u->host) return -1;
    memcpy(u->host, p, host_len);
    u->host[host_len] = '\0';
    p += host_len;
    if (*p == ':') {
        char *end = NULL;
        unsigned long port = strtoul(p + 1, &end, 10);
        if (port == 0 || port > 65535) return -1;
        u->port = (unsigned short)port;
        p = end;
    }
    const char *prefix = (*p == '/') ? "" : "/";
    int n = snprintf(u->path, sizeof u->path, "%s%s", prefix, p);
    return (n < 0 || (size_t)n >= sizeof u->path) ? -1 : 0;
}

static int tcp_connect(const char *host, unsigned short port)
{
    char res_buf[1024];
    int rid = -1;
    int ret = sceNetResolverCreate(&rid, res_buf, sizeof res_buf);
    if (ret < 0) return ret;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = PSP_HTONS(port);
    ret = sceNetResolverStartNtoA(rid, host, &sin.sin_addr, 5, 3);
    sceNetResolverDelete(rid);
    if (ret < 0) {
        log_printf("HTTP: DNS %s failed 0x%08X\n", host, (unsigned int)ret);
        return ret;
    }

    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return sock;
    unsigned int timeout = SOCK_TIMEOUT_US;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    ret = sceNetInetConnect(sock, (struct sockaddr *)&sin, sizeof sin);
    if (ret < 0) {
        log_printf("HTTP: connect %s:%u failed %d\n", host, (unsigned int)port, ret);
        sceNetInetClose(sock);
        return ret;
    }
    return sock;
}

static int transport_recv(http_stream *s, void *buf, size_t len)
{
    if (s->tls) return tls_read(s->tls, buf, len);
    int r = sceNetInetRecv(s->sock, buf, len, 0);
    return r < 0 ? -1 : r;
}

static int transport_send_all(http_stream *s, const void *buf, size_t len)
{
    if (len == 0) return 0;
    if (s->tls) return tls_write_all(s->tls, buf, len);
    const unsigned char *p = buf;
    while (len > 0) {
        int w = sceNetInetSend(s->sock, p, len, 0);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Returns 1 when buffered bytes are available, 0 at EOF, < 0 on error. */
static int fill(http_stream *s)
{
    if (s->buf_pos < s->buf_len) return 1;
    int r = transport_recv(s, s->buf, sizeof s->buf);
    if (r <= 0) return r;
    s->buf_pos = 0;
    s->buf_len = (size_t)r;
    return 1;
}

/* Reads one line, strips CRLF. Over-long lines are truncated. */
static int read_line(http_stream *s, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (fill(s) <= 0) return -1;
        char ch = (char)s->buf[s->buf_pos++];
        if (ch == '\n') {
            if (n > 0 && line[n - 1] == '\r') --n;
            line[n] = '\0';
            return (int)n;
        }
        if (n + 1 < cap) line[n++] = ch;
    }
}

void http_stream_close(http_stream *s)
{
    if (s->tls) tls_close(s->tls);
    if (s->sock >= 0) sceNetInetClose(s->sock);
    s->tls = NULL;
    s->sock = -1;
}

static int open_once(http_stream *s, const char *method, const url_parts *u,
                     const char *extra_headers, const void *body, size_t body_len,
                     uint64_t *content_length, char *location, size_t location_cap)
{
    memset(s, 0, sizeof *s);
    s->sock = -1;
    location[0] = '\0';

    s->sock = tcp_connect(u->host, u->port);
    if (s->sock < 0) return s->sock;
    if (u->https) {
        s->tls = tls_open(s->sock, u->host);
        if (!s->tls) return -3;
    }

    char req[2048];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: PSP-Spotify/1.0\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Connection: close\r\n",
                     method, u->path, u->host);
    if (n < 0 || (size_t)n >= sizeof req) return -4;
    size_t used = (size_t)n;
    if (body || strcmp(method, "POST") == 0) {
        n = snprintf(req + used, sizeof req - used, "Content-Length: %u\r\n",
                     (unsigned int)body_len);
        if (n < 0 || (size_t)n >= sizeof req - used) return -4;
        used += (size_t)n;
    }
    n = snprintf(req + used, sizeof req - used, "%s\r\n", extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof req - used) return -4;
    used += (size_t)n;

    if (transport_send_all(s, req, used) < 0 || transport_send_all(s, body, body_len) < 0) {
        log_printf("HTTP: send failed\n");
        return -5;
    }

    char line[1024];
    if (read_line(s, line, sizeof line) < 0) {
        log_printf("HTTP: no status line\n");
        return -6;
    }
    const char *sp = strchr(line, ' ');
    int status = sp ? (int)strtol(sp + 1, NULL, 10) : 0;
    if (strncmp(line, "HTTP/", 5) != 0 || status <= 0) {
        log_printf("HTTP: bad status line: %.60s\n", line);
        return -6;
    }

    int have_length = 0;
    for (;;) {
        int len = read_line(s, line, sizeof line);
        if (len < 0) return -7;
        if (len == 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            *content_length = strtoull(line + 15, NULL, 10);
            have_length = 1;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                   strstr(line + 18, "chunked")) {
            s->chunked = 1;
        } else if (strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ' || *v == '\t') ++v;
            int ln = snprintf(location, location_cap, "%s", v);
            if (ln < 0 || (size_t)ln >= location_cap) location[0] = '\0';
        }
    }

    if (s->chunked) {
        *content_length = 0;
        s->remaining = 0;
    } else if (have_length) {
        s->remaining = *content_length;
    } else {
        s->remaining = BODY_UNTIL_CLOSE;
    }
    return status;
}

int http_stream_open_ex(http_stream *s, const char *method, const char *url,
                        const char *extra_headers, const void *body, size_t body_len,
                        uint64_t *content_length)
{
    static url_parts u;
    static char location[1024];
    *content_length = 0;
    s->sock = -1;
    s->tls = NULL;
    if (parse_url(url, &u) < 0) {
        log_printf("HTTP: bad URL\n");
        return -1;
    }

    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        int status = open_once(s, method, &u, extra_headers, body, body_len,
                               content_length, location, sizeof location);
        if (status < 0) {
            http_stream_close(s);
            return status;
        }
        int redirect = status == 301 || status == 302 || status == 303 ||
                       status == 307 || status == 308;
        if (!redirect || location[0] == '\0') return status;

        http_stream_close(s);
        log_printf("HTTP: %d -> %.70s\n", status, location);
        if (location[0] == '/') {
            int n = snprintf(u.path, sizeof u.path, "%s", location);
            if (n < 0 || (size_t)n >= sizeof u.path) return -1;
        } else if (parse_url(location, &u) < 0) {
            log_printf("HTTP: bad redirect URL\n");
            return -1;
        }
        if (status == 303) {
            method = "GET";
            body = NULL;
            body_len = 0;
        }
    }
    log_printf("HTTP: too many redirects\n");
    return -8;
}

int http_stream_open(http_stream *s, const char *url, uint64_t *content_length)
{
    return http_stream_open_ex(s, "GET", url, NULL, NULL, 0, content_length);
}

int http_stream_read(http_stream *s, void *out, unsigned int len)
{
    if (s->body_done) return 0;
    if (s->chunked && s->remaining == 0) {
        char line[64];
        /* Each chunk's data is followed by CRLF before the next size line. */
        if (s->chunk_started && read_line(s, line, sizeof line) < 0) return -1;
        if (read_line(s, line, sizeof line) < 0) return -1;
        s->chunk_started = 1;
        s->remaining = strtoull(line, NULL, 16);
        if (s->remaining == 0) {
            s->body_done = 1;
            return 0;
        }
    }
    if (s->remaining == 0) {
        s->body_done = 1;
        return 0;
    }

    size_t want = len;
    if (want > s->remaining) want = (size_t)s->remaining;
    int got;
    if (s->buf_pos < s->buf_len) {
        size_t avail = s->buf_len - s->buf_pos;
        if (want > avail) want = avail;
        memcpy(out, s->buf + s->buf_pos, want);
        s->buf_pos += want;
        got = (int)want;
    } else {
        got = transport_recv(s, out, want);
        if (got == 0) {
            if (s->remaining == BODY_UNTIL_CLOSE) {
                s->body_done = 1;
                return 0;
            }
            log_printf("HTTP: body truncated\n");
            return -1;
        }
        if (got < 0) return -1;
    }
    if (s->remaining != BODY_UNTIL_CLOSE) s->remaining -= (uint64_t)got;
    return got;
}
