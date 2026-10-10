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

/* Minimal protobuf reader: iterate the fields of one message. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} pb_reader;

typedef struct {
    uint32_t field;
    uint32_t wire;
    uint64_t varint;       /* wire type 0 */
    const uint8_t *data;   /* wire type 2 */
    size_t len;            /* wire type 2 */
} pb_field;

static inline void pb_init(pb_reader *r, const uint8_t *data, size_t len)
{
    r->p = data;
    r->end = data + len;
}

static inline int pb_read_varint(pb_reader *r, uint64_t *out)
{
    uint64_t v = 0;
    for (unsigned int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return -1;
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

/* Returns 1 with the next field, 0 at end of message, -1 on malformed data. */
static inline int pb_next(pb_reader *r, pb_field *f)
{
    if (r->p >= r->end) return 0;
    uint64_t key;
    if (pb_read_varint(r, &key) < 0) return -1;
    f->field = (uint32_t)(key >> 3);
    f->wire = (uint32_t)(key & 7);
    f->data = NULL;
    f->len = 0;
    f->varint = 0;
    switch (f->wire) {
    case 0:
        return pb_read_varint(r, &f->varint) < 0 ? -1 : 1;
    case 1:
        if (r->end - r->p < 8) return -1;
        f->data = r->p; f->len = 8; r->p += 8;
        return 1;
    case 2: {
        uint64_t len;
        if (pb_read_varint(r, &len) < 0 || len > (uint64_t)(r->end - r->p)) return -1;
        f->data = r->p; f->len = (size_t)len; r->p += len;
        return 1;
    }
    case 5:
        if (r->end - r->p < 4) return -1;
        f->data = r->p; f->len = 4; r->p += 4;
        return 1;
    default:
        return -1;
    }
}

/* Finds the first length-delimited field `field` in a message. */
static inline int pb_find_bytes(const uint8_t *msg, size_t len, uint32_t field,
                                const uint8_t **out, size_t *out_len)
{
    pb_reader r;
    pb_field f;
    pb_init(&r, msg, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == field && f.wire == 2) {
            *out = f.data;
            *out_len = f.len;
            return 0;
        }
    }
    return -1;
}

#endif /* SPOTIFY_PROTO_UTIL_H */
