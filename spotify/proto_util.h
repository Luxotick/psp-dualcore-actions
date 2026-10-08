#ifndef SPOTIFY_PROTO_UTIL_H
#define SPOTIFY_PROTO_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} buf_writer;

static inline void bw_put_byte(buf_writer *w, uint8_t b)
{
    if (w->len < w->cap) {
        w->buf[w->len++] = b;
    }
}

static inline void bw_put_varint(buf_writer *w, uint64_t v)
{
    while (v >= 0x80) {
        bw_put_byte(w, (uint8_t)((v & 0x7F) | 0x80));
        v >>= 7;
    }
    bw_put_byte(w, (uint8_t)v);
}

static inline void bw_put_tag(buf_writer *w, uint32_t field, uint32_t wire)
{
    bw_put_varint(w, ((uint64_t)field << 3) | wire);
}

static inline void bw_put_bytes(buf_writer *w, uint32_t field, const uint8_t *data, size_t len)
{
    bw_put_tag(w, field, 2);
    bw_put_varint(w, (uint64_t)len);
    for (size_t i = 0; i < len; ++i) {
        bw_put_byte(w, data[i]);
    }
}

static inline void bw_put_string(buf_writer *w, uint32_t field, const char *str)
{
    if (str) {
        bw_put_bytes(w, field, (const uint8_t *)str, strlen(str));
    }
}

static inline void bw_put_varint_field(buf_writer *w, uint32_t field, uint64_t v)
{
    bw_put_tag(w, field, 0);
    bw_put_varint(w, v);
}

#endif /* SPOTIFY_PROTO_UTIL_H */
