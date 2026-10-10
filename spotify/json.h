#ifndef SPOTIFY_JSON_H
#define SPOTIFY_JSON_H

#include <stddef.h>

/* Read-only view of one parsed JSON document (jsmn tokens in a static pool,
 * so only one document is alive at a time). Token 0 is the root. */
typedef struct {
    const char *js;
    int count;
} json_doc;

int json_parse(json_doc *d, const char *js, size_t len);
/* Value token of `key` in object token `obj`, or -1. */
int json_get(const json_doc *d, int obj, const char *key);
/* Follows a dotted path of object keys, e.g. "track.album.name". */
int json_path(const json_doc *d, int obj, const char *path);
/* Number of elements of array token `arr` (0 if not an array). */
int json_array_len(const json_doc *d, int arr);
/* Token of element `n` of array `arr`, or -1. */
int json_array_at(const json_doc *d, int arr, int n);
/* Copies a string token, decoding escapes to UTF-8; truncates safely. */
int json_string(const json_doc *d, int tok, char *out, size_t cap);
long json_long(const json_doc *d, int tok, long fallback);
int json_is_null(const json_doc *d, int tok);

#endif /* SPOTIFY_JSON_H */
