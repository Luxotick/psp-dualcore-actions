#include "json.h"
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#include "jsmn.h"

/* 16384 tokens cover a 50-item Web API page with room to spare. */
static jsmntok_t tokens[16384];

int json_parse(json_doc *d, const char *js, size_t len)
{
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, tokens, sizeof tokens / sizeof tokens[0]);
    d->js = js;
    d->count = n > 0 ? n : 0;
    return n;
}

/* Index of the first token after the subtree rooted at i. */
static int skip(const json_doc *d, int i)
{
    int pending = 1;
    while (pending > 0 && i < d->count) {
        const jsmntok_t *t = &tokens[i];
        if (t->type == JSMN_OBJECT) pending += t->size * 2;
        else if (t->type == JSMN_ARRAY) pending += t->size;
        --pending;
        ++i;
    }
    return i;
}

static int token_equals(const json_doc *d, int i, const char *s, size_t len)
{
    const jsmntok_t *t = &tokens[i];
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == len &&
           memcmp(d->js + t->start, s, len) == 0;
}

static int get_n(const json_doc *d, int obj, const char *key, size_t key_len)
{
    if (obj < 0 || obj >= d->count || tokens[obj].type != JSMN_OBJECT) return -1;
    int i = obj + 1;
    for (int k = 0; k < tokens[obj].size && i + 1 < d->count; ++k) {
        if (token_equals(d, i, key, key_len)) return i + 1;
        i = skip(d, i + 1);
    }
    return -1;
}

int json_get(const json_doc *d, int obj, const char *key)
{
    return get_n(d, obj, key, strlen(key));
}

int json_path(const json_doc *d, int obj, const char *path)
{
    while (obj >= 0 && *path) {
        const char *dot = strchr(path, '.');
        size_t len = dot ? (size_t)(dot - path) : strlen(path);
        obj = get_n(d, obj, path, len);
        path += len + (dot ? 1 : 0);
    }
    return obj;
}

int json_array_len(const json_doc *d, int arr)
{
    if (arr < 0 || arr >= d->count || tokens[arr].type != JSMN_ARRAY) return 0;
    return tokens[arr].size;
}

int json_array_at(const json_doc *d, int arr, int n)
{
    if (n < 0 || n >= json_array_len(d, arr)) return -1;
    int i = arr + 1;
    for (int k = 0; k < n; ++k) i = skip(d, i);
    return i < d->count ? i : -1;
}

static size_t put_utf8(char *out, size_t pos, size_t cap, unsigned int cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
           b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    if (pos + n >= cap) return pos;
    memcpy(out + pos, b, n);
    return pos + n;
}

int json_string(const json_doc *d, int tok, char *out, size_t cap)
{
    if (cap == 0) return -1;
    out[0] = '\0';
    if (tok < 0 || tok >= d->count || tokens[tok].type != JSMN_STRING) return -1;
    const char *s = d->js + tokens[tok].start;
    const char *end = d->js + tokens[tok].end;
    size_t pos = 0;
    while (s < end && pos + 1 < cap) {
        char c = *s++;
        if (c != '\\' || s >= end) { out[pos++] = c; continue; }
        c = *s++;
        switch (c) {
        case 'n': out[pos++] = ' '; break;
        case 't': out[pos++] = ' '; break;
        case 'u': {
            if (end - s < 4) { s = end; break; }
            char hex[5] = { s[0], s[1], s[2], s[3], 0 };
            s += 4;
            /* Surrogate pairs (emoji) are outside the debug font: show '?'. */
            unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (cp <= 0xDBFF && end - s >= 6 && s[0] == '\\' && s[1] == 'u') s += 6;
                cp = '?';
            }
            pos = put_utf8(out, pos, cap, cp);
            break;
        }
        default: out[pos++] = c; break;   /* \" \\ \/ and the rest */
        }
    }
    out[pos] = '\0';
    return 0;
}

long json_long(const json_doc *d, int tok, long fallback)
{
    if (tok < 0 || tok >= d->count || tokens[tok].type != JSMN_PRIMITIVE) return fallback;
    char c = d->js[tokens[tok].start];
    if (c != '-' && (c < '0' || c > '9')) return fallback;
    return strtol(d->js + tokens[tok].start, NULL, 10);
}

int json_is_null(const json_doc *d, int tok)
{
    return tok < 0 || tok >= d->count ||
           (tokens[tok].type == JSMN_PRIMITIVE && d->js[tokens[tok].start] == 'n');
}
