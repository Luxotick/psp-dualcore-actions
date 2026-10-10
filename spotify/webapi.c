#include "webapi.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "proto_util.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspthreadman.h>

/* Desktop Linux identity as used by librespot: with the keymaster client id
 * the client token is granted without a hashcash challenge. */
#define SPOTIFY_CLIENT_VERSION "1.2.52.442"
#define SPCLIENT "https://spclient.wg.spotify.com"

/* Large enough for a 500-track playlist or a 120-track metadata batch
 * (each TRACK_V4 record is a few KB: album, covers, files, restrictions). */
static uint8_t resp_buf[1024 * 1024];
static char headers[1536];

const char *spotify_format_name(int format)
{
    static const char *const names[] = {
        "OGG_VORBIS_96", "OGG_VORBIS_160", "OGG_VORBIS_320", "MP3_256", "MP3_320",
        "MP3_160", "MP3_96", "MP3_160_ENC", "AAC_24", "AAC_48"
    };
    if (format >= 0 && format < (int)(sizeof names / sizeof names[0])) return names[format];
    if (format == 16) return "FLAC_FLAC";
    if (format == 22) return "FLAC_FLAC_24BIT";
    return "OTHER";
}

int spotify_base62_to_gid(const char *id, uint8_t gid[SPOTIFY_GID_LEN])
{
    static const char alphabet[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    memset(gid, 0, SPOTIFY_GID_LEN);
    for (const char *c = id; *c; ++c) {
        const char *pos = strchr(alphabet, *c);
        if (!pos) return -1;
        unsigned int carry = (unsigned int)(pos - alphabet);
        for (int i = SPOTIFY_GID_LEN - 1; i >= 0; --i) {
            unsigned int v = (unsigned int)gid[i] * 62u + carry;
            gid[i] = (uint8_t)v;
            carry = v >> 8;
        }
        if (carry) return -1;
    }
    return 0;
}

/* Sends one request and reads the whole body into resp_buf. Returns the
 * HTTP status (body length in *out_len) or a negative error. */
static int request_all(const char *method, const char *url, const char *extra_headers,
                       const uint8_t *body, size_t body_len, size_t *out_len)
{
    http_stream s;
    uint64_t content_length = 0;
    *out_len = 0;
    int status = http_stream_open_ex(&s, method, url, extra_headers, body, body_len,
                                     &content_length);
    if (status < 0) return status;
    size_t total = 0;
    for (;;) {
        if (total == sizeof resp_buf) {
            log_printf("API: response too large\n");
            http_stream_close(&s);
            return -20;
        }
        int r = http_stream_read(&s, resp_buf + total, (unsigned int)(sizeof resp_buf - total));
        if (r < 0) {
            http_stream_close(&s);
            return -21;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    http_stream_close(&s);
    *out_len = total;
    /* Keep text responses (JSON) NUL-terminated for the parsers below. */
    resp_buf[total < sizeof resp_buf ? total : sizeof resp_buf - 1] = 0;
    return status;
}

/* Copies the string value of "key" from a flat JSON object. */
static int flat_json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[48];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof pattern) return -1;
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return -1;
    ++p;
    size_t len = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (len + 1 >= cap) return -1;
        out[len++] = *p++;
    }
    out[len] = '\0';
    return *p == '"' ? 0 : -1;
}

static long flat_json_int(const char *json, const char *key, long fallback)
{
    char pattern[48];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof pattern) return fallback;
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p += n;
    while (*p == ' ' || *p == ':') ++p;
    return (*p >= '0' && *p <= '9') ? strtol(p, NULL, 10) : fallback;
}

/* The scopes librespot requests (OAUTH_SCOPES), space separated ('+'). */
#define OAUTH_SCOPES \
    "app-remote-control+playlist-modify+playlist-modify-private+playlist-modify-public+" \
    "playlist-read+playlist-read-collaborative+playlist-read-private+streaming+" \
    "ugc-image-upload+user-follow-modify+user-follow-read+user-library-modify+" \
    "user-library-read+user-modify+user-modify-playback-state+user-modify-private+" \
    "user-personalized+user-read-birthdate+user-read-currently-playing+user-read-email+" \
    "user-read-play-history+user-read-playback-position+user-read-playback-state+" \
    "user-read-private+user-read-recently-played+user-top-read"
#define FORM_HEADERS "Accept: application/json\r\n" \
                     "Content-Type: application/x-www-form-urlencoded\r\n"

static int token_response(char *access_token, size_t access_cap,
                          char *refresh_token, size_t refresh_cap)
{
    const char *json = (const char *)resp_buf;
    if (flat_json_string(json, "access_token", access_token, access_cap) < 0) return -1;
    if (refresh_token && refresh_cap) {
        refresh_token[0] = '\0';
        flat_json_string(json, "refresh_token", refresh_token, refresh_cap);
    }
    log_printf("OAUTH: access token ok (%u chars, expires in %ld s)\n",
               (unsigned int)strlen(access_token), flat_json_int(json, "expires_in", 0));
    return 0;
}

static int spotify_oauth_device_pair_unlocked(char *access_token, size_t access_cap,
                              char *refresh_token, size_t refresh_cap,
                              void (*show_code)(const char *user_code))
{
    static char body[1024];
    static char device_code[256];
    char user_code[32], error[64];
    int n = snprintf(body, sizeof body, "client_id=%s&scope=%s",
                     SPOTIFY_CLIENT_ID_KEYMASTER, OAUTH_SCOPES);
    if (n < 0 || (size_t)n >= sizeof body) return -1;

    size_t len = 0;
    int status = request_all("POST", "https://accounts.spotify.com/oauth2/device/authorize",
                             FORM_HEADERS, (const uint8_t *)body, (size_t)n, &len);
    const char *json = (const char *)resp_buf;
    if (status != 200 || flat_json_string(json, "device_code", device_code, sizeof device_code) < 0 ||
        flat_json_string(json, "user_code", user_code, sizeof user_code) < 0) {
        error[0] = '\0';
        flat_json_string(json, "error", error, sizeof error);
        log_printf("PAIR: device authorize failed (HTTP %d %s)\n", status, error);
        return -2;
    }
    long interval = flat_json_int(json, "interval", 5);
    long expires = flat_json_int(json, "expires_in", 600);
    if (interval < 1) interval = 5;
    if (expires > 900) expires = 900;

    log_printf("\n========================================\n");
    log_printf("  PAIR THIS PSP WITH SPOTIFY:\n");
    log_printf("  open  spotify.com/pair  on your phone\n");
    log_printf("  and enter the code:  %s\n", user_code);
    log_printf("========================================\n");
    if (show_code) show_code(user_code);

    n = snprintf(body, sizeof body,
                 "client_id=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code"
                 "&device_code=%s", SPOTIFY_CLIENT_ID_KEYMASTER, device_code);
    if (n < 0 || (size_t)n >= sizeof body) return -1;

    for (long waited = 0; waited < expires; waited += interval) {
        sceKernelDelayThread((SceUInt)(interval * 1000000L));
        status = request_all("POST", "https://accounts.spotify.com/api/token", FORM_HEADERS,
                             (const uint8_t *)body, (size_t)n, &len);
        if (status == 200)
            return token_response(access_token, access_cap, refresh_token, refresh_cap);
        error[0] = '\0';
        flat_json_string(json, "error", error, sizeof error);
        if (strcmp(error, "authorization_pending") == 0) continue;
        if (strcmp(error, "slow_down") == 0) { interval += 5; continue; }
        log_printf("PAIR: token poll failed (HTTP %d %s)\n", status, error);
        return -3;
    }
    log_printf("PAIR: code expired\n");
    return -4;
}

static int spotify_oauth_refresh_unlocked(const char *refresh_token, char *access_token, size_t access_cap,
                          char *new_refresh, size_t refresh_cap)
{
    static char body[1024];
    int n = snprintf(body, sizeof body, "client_id=%s&grant_type=refresh_token&refresh_token=%s",
                     SPOTIFY_CLIENT_ID_KEYMASTER, refresh_token);
    if (n < 0 || (size_t)n >= sizeof body) return -1;
    size_t len = 0;
    int status = request_all("POST", "https://accounts.spotify.com/api/token", FORM_HEADERS,
                             (const uint8_t *)body, (size_t)n, &len);
    if (status != 200) {
        char error[64] = "";
        flat_json_string((const char *)resp_buf, "error", error, sizeof error);
        log_printf("OAUTH: refresh failed (HTTP %d %s)\n", status, error);
        return -2;
    }
    return token_response(access_token, access_cap, new_refresh, refresh_cap);
}

static int spotify_client_token_unlocked(const char *client_id, char *out, size_t cap)
{

    uint8_t linux_buf[64], platform_buf[80], conn_buf[160], data_buf[256], req_buf[300];
    buf_writer linux_w = { linux_buf, sizeof linux_buf, 0 };
    bw_put_string(&linux_w, 1, "Linux");
    bw_put_string(&linux_w, 2, "0");
    bw_put_string(&linux_w, 3, "0");
    bw_put_string(&linux_w, 4, "mips");
    buf_writer platform_w = { platform_buf, sizeof platform_buf, 0 };
    bw_put_bytes(&platform_w, 5, linux_buf, linux_w.len);          /* desktop_linux */
    buf_writer conn_w = { conn_buf, sizeof conn_buf, 0 };
    bw_put_bytes(&conn_w, 1, platform_buf, platform_w.len);
    bw_put_string(&conn_w, 2, SPOTIFY_DEVICE_ID);
    buf_writer data_w = { data_buf, sizeof data_buf, 0 };
    bw_put_string(&data_w, 1, SPOTIFY_CLIENT_VERSION);
    bw_put_string(&data_w, 2, client_id);
    bw_put_bytes(&data_w, 3, conn_buf, conn_w.len);                /* connectivity_sdk_data */
    buf_writer req_w = { req_buf, sizeof req_buf, 0 };
    bw_put_varint_field(&req_w, 1, 1);                             /* REQUEST_CLIENT_DATA_REQUEST */
    bw_put_bytes(&req_w, 2, data_buf, data_w.len);
    if (req_w.len == req_w.cap) return -1;

    size_t len = 0;
    int status = request_all("POST", "https://clienttoken.spotify.com/v1/clienttoken",
                             "Accept: application/x-protobuf\r\n"
                             "Content-Type: application/x-protobuf\r\n",
                             req_buf, req_w.len, &len);
    if (status != 200) {
        log_printf("CLIENTTOKEN: HTTP %d\n", status);
        return status < 0 ? status : -status;
    }

    pb_reader r;
    pb_field f;
    uint64_t response_type = 0;
    const uint8_t *granted = NULL;
    size_t granted_len = 0;
    pb_init(&r, resp_buf, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1 && f.wire == 0) response_type = f.varint;
        if (f.field == 2 && f.wire == 2) { granted = f.data; granted_len = f.len; }
    }
    if (response_type != 1 || !granted) {
        /* A hashcash challenge (type 2) has not been seen for this client id. */
        log_printf("CLIENTTOKEN: response type %u not granted\n", (unsigned int)response_type);
        return -2;
    }
    const uint8_t *tok;
    size_t tok_len;
    if (pb_find_bytes(granted, granted_len, 1, &tok, &tok_len) < 0 || tok_len + 1 > cap)
        return -3;
    memcpy(out, tok, tok_len);
    out[tok_len] = '\0';
    return 0;
}

static int trailing_zero_bits(const uint8_t be64[8])
{
    int n = 0;
    for (int i = 7; i >= 0; --i) {
        if (be64[i] == 0) { n += 8; continue; }
        uint8_t b = be64[i];
        while (!(b & 1)) { ++n; b >>= 1; }
        break;
    }
    return n;
}

/* login5 hashcash: suffix = BE64(target + n) || BE64(n), where target is
 * bytes 12..19 of SHA1(login_context); SHA1(prefix || suffix) bytes 12..19
 * must end in `length` zero bits. */
static int solve_hashcash(const uint8_t *ctx, size_t ctx_len, const uint8_t *prefix,
                          size_t prefix_len, int length, uint8_t suffix[16],
                          uint32_t *elapsed_us)
{
    uint8_t md[20];
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, ctx, ctx_len);
    sha1_final(&c, md);
    uint64_t target = 0;
    for (int i = 12; i < 20; ++i) target = (target << 8) | md[i];

    SceInt64 t0 = sceKernelGetSystemTimeWide();
    for (uint64_t n = 0; n < 50000000u; ++n) {
        uint64_t a = target + n;
        for (int i = 0; i < 8; ++i) {
            suffix[i] = (uint8_t)(a >> (56 - 8 * i));
            suffix[8 + i] = (uint8_t)(n >> (56 - 8 * i));
        }
        sha1_init(&c);
        sha1_update(&c, prefix, prefix_len);
        sha1_update(&c, suffix, 16);
        sha1_final(&c, md);
        if (trailing_zero_bits(&md[12]) >= length) {
            *elapsed_us = (uint32_t)(sceKernelGetSystemTimeWide() - t0);
            return 0;
        }
    }
    return -1;
}

static int spotify_login5_unlocked(const char *client_id, const char *client_token, const char *username,
                   const uint8_t *stored_credential, size_t stored_credential_len,
                   char *access_token, size_t cap)
{

    static uint8_t login_context[512];
    size_t login_context_len = 0;
    uint8_t solution_buf[128];
    size_t solution_len = 0;

    int n = snprintf(headers, sizeof headers,
                     "Accept: application/x-protobuf\r\n"
                     "Content-Type: application/x-protobuf\r\n"
                     "client-token: %s\r\n", client_token);
    if (n < 0 || (size_t)n >= sizeof headers) return -1;

    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t info_buf[128], cred_buf[1200];
        static uint8_t req_buf[2048];
        buf_writer info_w = { info_buf, sizeof info_buf, 0 };
        bw_put_string(&info_w, 1, client_id);
        bw_put_string(&info_w, 2, SPOTIFY_DEVICE_ID);
        buf_writer cred_w = { cred_buf, sizeof cred_buf, 0 };
        bw_put_string(&cred_w, 1, username);
        bw_put_bytes(&cred_w, 2, stored_credential, stored_credential_len);
        buf_writer req_w = { req_buf, sizeof req_buf, 0 };
        bw_put_bytes(&req_w, 1, info_buf, info_w.len);                 /* client_info */
        if (login_context_len) {
            bw_put_bytes(&req_w, 2, login_context, login_context_len);
            bw_put_bytes(&req_w, 3, solution_buf, solution_len);      /* challenge_solutions */
        }
        bw_put_bytes(&req_w, 100, cred_buf, cred_w.len);              /* stored_credential */
        if (req_w.len == req_w.cap || cred_w.len == cred_w.cap) return -1;

        size_t len = 0;
        int status = request_all("POST", "https://login5.spotify.com/v3/login", headers,
                                 req_buf, req_w.len, &len);
        if (status != 200) {
            log_printf("LOGIN5: HTTP %d\n", status);
            return status < 0 ? status : -status;
        }

        pb_reader r;
        pb_field f;
        const uint8_t *ok = NULL, *challenges = NULL;
        size_t ok_len = 0, challenges_len = 0;
        int error = -1;
        pb_init(&r, resp_buf, len);
        while (pb_next(&r, &f) == 1) {
            if (f.field == 1 && f.wire == 2) { ok = f.data; ok_len = f.len; }
            else if (f.field == 2 && f.wire == 0) error = (int)f.varint;
            else if (f.field == 3 && f.wire == 2) { challenges = f.data; challenges_len = f.len; }
            else if (f.field == 5 && f.wire == 2 && f.len <= sizeof login_context) {
                memcpy(login_context, f.data, f.len);
                login_context_len = f.len;
            }
        }
        if (ok) {
            const uint8_t *tok;
            size_t tok_len;
            if (pb_find_bytes(ok, ok_len, 2, &tok, &tok_len) < 0 || tok_len + 1 > cap) return -3;
            memcpy(access_token, tok, tok_len);
            access_token[tok_len] = '\0';
            pb_init(&r, ok, ok_len);
            while (pb_next(&r, &f) == 1)
                if (f.field == 4 && f.wire == 0)
                    log_printf("LOGIN5: access token ok (%u chars, expires in %u s)\n",
                               (unsigned int)tok_len, (unsigned int)f.varint);
            return 0;
        }
        if (error >= 0) {
            /* 1 INVALID_CREDENTIALS, 2 BAD_REQUEST, 3 UNSUPPORTED_LOGIN_PROTOCOL,
             * 4 TIMEOUT, 5 UNKNOWN_IDENTIFIER, 6 TOO_MANY_ATTEMPTS, 8 TRY_AGAIN_LATER */
            log_printf("LOGIN5: error %d\n", error);
            if (error != 4 && error != 6) return -10 - error;
            sceKernelDelayThread(3 * 1000 * 1000);
            continue;
        }
        if (!challenges || !login_context_len) {
            log_printf("LOGIN5: unexpected response (%u bytes)\n", (unsigned int)len);
            return -4;
        }

        /* Challenges { repeated Challenge challenges = 1 }, Challenge { hashcash = 1 } */
        const uint8_t *challenge, *hashcash, *prefix = NULL;
        size_t challenge_len, hashcash_len, prefix_len = 0;
        if (pb_find_bytes(challenges, challenges_len, 1, &challenge, &challenge_len) < 0 ||
            pb_find_bytes(challenge, challenge_len, 1, &hashcash, &hashcash_len) < 0) {
            log_printf("LOGIN5: unsupported challenge (not hashcash)\n");
            return -5;
        }
        int bits = 0;
        pb_init(&r, hashcash, hashcash_len);
        while (pb_next(&r, &f) == 1) {
            if (f.field == 1 && f.wire == 2) { prefix = f.data; prefix_len = f.len; }
            if (f.field == 2 && f.wire == 0) bits = (int)f.varint;
        }
        uint8_t suffix[16];
        uint32_t elapsed_us = 0;
        if (!prefix || solve_hashcash(login_context, login_context_len, prefix, prefix_len,
                                      bits, suffix, &elapsed_us) < 0) {
            log_printf("LOGIN5: hashcash failed\n");
            return -6;
        }
        log_printf("LOGIN5: hashcash %d bits solved in %u ms\n", bits,
                   (unsigned int)(elapsed_us / 1000));

        /* ChallengeSolutions { solutions = 1 { hashcash = 1 { suffix = 1, duration = 2 } } } */
        uint8_t dur_buf[16], hs_buf[48], sol_buf[64];
        buf_writer dur_w = { dur_buf, sizeof dur_buf, 0 };
        bw_put_varint_field(&dur_w, 1, elapsed_us / 1000000u);
        bw_put_varint_field(&dur_w, 2, (elapsed_us % 1000000u) * 1000u);
        buf_writer hs_w = { hs_buf, sizeof hs_buf, 0 };
        bw_put_bytes(&hs_w, 1, suffix, sizeof suffix);
        bw_put_bytes(&hs_w, 2, dur_buf, dur_w.len);
        buf_writer sol_w = { sol_buf, sizeof sol_buf, 0 };
        bw_put_bytes(&sol_w, 1, hs_buf, hs_w.len);
        buf_writer solutions_w = { solution_buf, sizeof solution_buf, 0 };
        bw_put_bytes(&solutions_w, 1, sol_buf, sol_w.len);
        solution_len = solutions_w.len;
    }
    return -7;
}

static int auth_headers(const char *client_token, const char *access_token)
{
    int n = snprintf(headers, sizeof headers,
                     "Accept: application/x-protobuf\r\n"
                     "Content-Type: application/x-protobuf\r\n"
                     "Authorization: Bearer %s\r\n"
                     "client-token: %s\r\n", access_token, client_token);
    return (n < 0 || (size_t)n >= sizeof headers) ? -1 : 0;
}

static int collect_files(const uint8_t *track, size_t track_len,
                         spotify_audio_file *files, int max_files, int depth)
{
    pb_reader r;
    pb_field f;
    int count = 0;
    pb_init(&r, track, track_len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 2 && f.wire == 2 && depth == 0)
            log_printf("TRACK: \"%.*s\"\n", (int)(f.len > 60 ? 60 : f.len), (const char *)f.data);
        if (f.field == 12 && f.wire == 2 && count < max_files) {
            pb_reader fr;
            pb_field ff;
            spotify_audio_file *af = &files[count];
            int have_id = 0;
            af->format = -1;
            pb_init(&fr, f.data, f.len);
            while (pb_next(&fr, &ff) == 1) {
                if (ff.field == 1 && ff.wire == 2 && ff.len == SPOTIFY_FILE_ID_LEN) {
                    memcpy(af->file_id, ff.data, SPOTIFY_FILE_ID_LEN);
                    have_id = 1;
                }
                if (ff.field == 2 && ff.wire == 0) af->format = (int)ff.varint;
            }
            if (have_id) ++count;
        }
    }
    if (count > 0 || depth > 0) return count;

    /* Region-restricted tracks list their files under alternatives (13). */
    pb_init(&r, track, track_len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 13 && f.wire == 2) {
            count = collect_files(f.data, f.len, files, max_files, depth + 1);
            if (count > 0) {
                log_printf("TRACK: using alternative\n");
                return count;
            }
        }
    }
    return 0;
}

static int spotify_track_files_unlocked(const char *client_token, const char *access_token,
                        const char *track_id, spotify_audio_file *files, int max_files)
{
    if (auth_headers(client_token, access_token) < 0) return -1;

    char uri[64];
    int n = snprintf(uri, sizeof uri, "spotify:track:%s", track_id);
    if (n < 0 || (size_t)n >= sizeof uri) return -1;

    /* BatchedEntityRequest { entity_request = 2 { entity_uri = 1,
     *   query = 2 { extension_kind = 1 (TRACK_V4 = 10) } } } */
    uint8_t query_buf[8], entity_buf[96], req_buf[112];
    buf_writer query_w = { query_buf, sizeof query_buf, 0 };
    bw_put_varint_field(&query_w, 1, 10);
    buf_writer entity_w = { entity_buf, sizeof entity_buf, 0 };
    bw_put_string(&entity_w, 1, uri);
    bw_put_bytes(&entity_w, 2, query_buf, query_w.len);
    buf_writer req_w = { req_buf, sizeof req_buf, 0 };
    bw_put_bytes(&req_w, 2, entity_buf, entity_w.len);

    size_t len = 0;
    int status = request_all("POST", SPCLIENT "/extended-metadata/v0/extended-metadata",
                             headers, req_buf, req_w.len, &len);
    if (status != 200) {
        log_printf("METADATA: HTTP %d\n", status);
        return status < 0 ? status : -status;
    }

    /* BatchedExtensionResponse.extended_metadata(2) -> EntityExtensionDataArray
     * .extension_data(3) -> EntityExtensionData.extension_data(3) (Any)
     * -> Any.value(2) = Track */
    const uint8_t *array, *data, *any, *track;
    size_t array_len, data_len, any_len, track_len;
    if (pb_find_bytes(resp_buf, len, 2, &array, &array_len) < 0 ||
        pb_find_bytes(array, array_len, 3, &data, &data_len) < 0) {
        log_printf("METADATA: no extension data (%u bytes)\n", (unsigned int)len);
        return -2;
    }
    const uint8_t *hdr;
    size_t hdr_len;
    if (pb_find_bytes(data, data_len, 1, &hdr, &hdr_len) == 0) {
        pb_reader r;
        pb_field f;
        pb_init(&r, hdr, hdr_len);
        while (pb_next(&r, &f) == 1)
            if (f.field == 1 && f.wire == 0 && f.varint != 200)
                log_printf("METADATA: entity status %u\n", (unsigned int)f.varint);
    }
    if (pb_find_bytes(data, data_len, 3, &any, &any_len) < 0 ||
        pb_find_bytes(any, any_len, 2, &track, &track_len) < 0) {
        log_printf("METADATA: empty track payload\n");
        return -3;
    }
    return collect_files(track, track_len, files, max_files, 0);
}

static int spotify_storage_resolve_unlocked(const char *client_token, const char *access_token,
                            const uint8_t file_id[SPOTIFY_FILE_ID_LEN],
                            char *cdn_urls, size_t url_cap, int max_urls)
{
    if (auth_headers(client_token, access_token) < 0) return -1;
    char url[160];
    int n = snprintf(url, sizeof url, SPCLIENT "/storage-resolve/files/audio/interactive/");
    for (int i = 0; i < SPOTIFY_FILE_ID_LEN && n > 0 && (size_t)n + 3 < sizeof url; ++i)
        n += snprintf(url + n, sizeof url - (size_t)n, "%02x", file_id[i]);

    size_t len = 0;
    int status = request_all("GET", url, headers, NULL, 0, &len);
    if (status != 200) {
        log_printf("STORAGE: HTTP %d\n", status);
        return status < 0 ? status : -status;
    }

    /* StorageResolveResponse { result = 1 (0 CDN, 1 STORAGE, 3 RESTRICTED),
     *                          repeated cdnurl = 2 } */
    pb_reader r;
    pb_field f;
    int result = 0, urls = 0, kept = 0;
    pb_init(&r, resp_buf, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1 && f.wire == 0) result = (int)f.varint;
        if (f.field == 2 && f.wire == 2) {
            if (kept < max_urls && f.len + 1 <= url_cap) {
                char *slot = cdn_urls + (size_t)kept * url_cap;
                memcpy(slot, f.data, f.len);
                slot[f.len] = '\0';
                ++kept;
            }
            ++urls;
        }
    }
    log_printf("STORAGE: result %d, %d CDN URLs\n", result, urls);
    return kept > 0 ? kept : -2;
}

/* ---------------------------------------------------------------- library */

static int sp_headers(const char *client_token, const char *access_token,
                      const char *content_type)
{
    int n = snprintf(headers, sizeof headers,
                     "Accept: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Authorization: Bearer %s\r\n"
                     "client-token: %s\r\n", content_type, content_type,
                     access_token, client_token);
    return (n < 0 || (size_t)n >= sizeof headers) ? -1 : 0;
}

/* "spotify:track:<22>" -> id; returns 0 for a playable track URI. */
static int track_uri_to_id(const uint8_t *uri, size_t len, char id[24])
{
    if (len != 36 || memcmp(uri, "spotify:track:", 14) != 0) return -1;
    memcpy(id, uri + 14, 22);
    id[22] = '\0';
    return 0;
}

static void copy_str(char *out, size_t cap, const uint8_t *data, size_t len)
{
    if (len + 1 > cap) len = cap - 1;
    memcpy(out, data, len);
    out[len] = '\0';
}

static int sp_get(const char *url, size_t *len)
{
    int status = request_all("GET", url, headers, NULL, 0, len);
    if (status != 200) log_printf("LIBRARY: HTTP %d for %.70s\n", status, url);
    return status;
}

static int sp_playlists_unlocked(const char *client_token, const char *access_token,
                                 const char *username, spotify_playlist *out, int max)
{
    static char url[256];
    if (sp_headers(client_token, access_token, "application/x-protobuf") < 0) return -1;
    snprintf(url, sizeof url, SPCLIENT "/playlist/v2/user/%s/rootlist"
             "?decorate=revision,attributes,length,owner,capabilities,status_code"
             "&from=0&length=%d", username, max * 2);
    size_t len = 0;
    int status = sp_get(url, &len);
    if (status != 200) return status < 0 ? status : -status;

    /* SelectedListContent.contents(5) = ListItems { items(3) Item{uri=1},
     * meta_items(4) MetaItem{attributes(2){name=1}, length(3)} }, the
     * meta items parallel to the items. */
    const uint8_t *contents;
    size_t contents_len;
    if (pb_find_bytes(resp_buf, len, 5, &contents, &contents_len) < 0) return 0;

    static const uint8_t *uris[512];
    static size_t uri_lens[512];
    static const uint8_t *metas[512];
    static size_t meta_lens[512];
    int n_items = 0, n_metas = 0;
    pb_reader r;
    pb_field f;
    pb_init(&r, contents, contents_len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 3 && f.wire == 2 && n_items < 512) {
            const uint8_t *uri;
            size_t uri_len;
            if (pb_find_bytes(f.data, f.len, 1, &uri, &uri_len) < 0) { uri = f.data; uri_len = 0; }
            uris[n_items] = uri;
            uri_lens[n_items++] = uri_len;
        } else if (f.field == 4 && f.wire == 2 && n_metas < 512) {
            metas[n_metas] = f.data;
            meta_lens[n_metas++] = f.len;
        }
    }
    int aligned = n_metas == n_items;
    int count = 0;
    for (int i = 0; i < n_items && count < max; ++i) {
        if (uri_lens[i] != 39 || memcmp(uris[i], "spotify:playlist:", 17) != 0)
            continue;                                  /* folder markers etc. */
        spotify_playlist *p = &out[count++];
        copy_str(p->id, sizeof p->id, uris[i] + 17, 22);
        snprintf(p->name, sizeof p->name, "Playlist %.8s", p->id);
        p->total = -1;
        if (!aligned) continue;
        pb_init(&r, metas[i], meta_lens[i]);
        while (pb_next(&r, &f) == 1) {
            if (f.field == 2 && f.wire == 2) {
                const uint8_t *name;
                size_t name_len;
                if (pb_find_bytes(f.data, f.len, 1, &name, &name_len) == 0)
                    copy_str(p->name, sizeof p->name, name, name_len);
            } else if (f.field == 3 && f.wire == 0) {
                p->total = (int)f.varint;
            }
        }
    }
    log_printf("LIBRARY: %d playlists (%d items, %d meta)\n", count, n_items, n_metas);
    return count;
}

static int sp_liked_unlocked(const char *client_token, const char *access_token,
                             const char *username, spotify_track *out, int max)
{
    static char token[256];
    static uint8_t body[512];
    token[0] = '\0';
    int count = 0;
    for (int page = 0; page < 50 && count < max; ++page) {
        if (sp_headers(client_token, access_token,
                       "application/vnd.collection-v2.spotify.proto") < 0) return -1;
        /* PageRequest { username = 1, set = 2, pagination_token = 3, limit = 4 } */
        buf_writer w = { body, sizeof body, 0 };
        bw_put_string(&w, 1, username);
        bw_put_string(&w, 2, "collection");
        if (token[0]) bw_put_string(&w, 3, token);
        bw_put_varint_field(&w, 4, 300);
        size_t len = 0;
        int status = request_all("POST", SPCLIENT "/collection/v2/paging", headers,
                                 body, w.len, &len);
        if (status != 200) {
            log_printf("LIBRARY: collection HTTP %d\n", status);
            return count > 0 ? count : (status < 0 ? status : -status);
        }
        /* PageResponse { items(1) CollectionItem{uri=1, is_removed=3},
         *                next_page_token = 2 } */
        pb_reader r;
        pb_field f;
        token[0] = '\0';
        int items = 0;
        pb_init(&r, resp_buf, len);
        while (pb_next(&r, &f) == 1) {
            if (f.field == 1 && f.wire == 2) {
                ++items;
                const uint8_t *uri = NULL;
                size_t uri_len = 0;
                int removed = 0;
                pb_reader ir;
                pb_field iff;
                pb_init(&ir, f.data, f.len);
                while (pb_next(&ir, &iff) == 1) {
                    if (iff.field == 1 && iff.wire == 2) { uri = iff.data; uri_len = iff.len; }
                    if (iff.field == 3 && iff.wire == 0) removed = (int)iff.varint;
                }
                if (uri && !removed && count < max &&
                    track_uri_to_id(uri, uri_len, out[count].id) == 0)
                    ++count;
            } else if (f.field == 2 && f.wire == 2) {
                copy_str(token, sizeof token, f.data, f.len);
            }
        }
        if (!token[0] || items == 0) break;
    }
    return count;
}

static int sp_tracks_unlocked(const char *client_token, const char *access_token,
                              const char *username, const char *playlist_id,
                              spotify_track *out, int max)
{
    if (!playlist_id[0]) return sp_liked_unlocked(client_token, access_token, username, out, max);
    static char url[160];
    if (sp_headers(client_token, access_token, "application/x-protobuf") < 0) return -1;
    snprintf(url, sizeof url, SPCLIENT "/playlist/v2/playlist/%s", playlist_id);
    size_t len = 0;
    int status = sp_get(url, &len);
    if (status != 200) return status < 0 ? status : -status;
    const uint8_t *contents;
    size_t contents_len;
    if (pb_find_bytes(resp_buf, len, 5, &contents, &contents_len) < 0) return 0;
    int count = 0;
    pb_reader r;
    pb_field f;
    pb_init(&r, contents, contents_len);
    while (pb_next(&r, &f) == 1 && count < max) {
        if (f.field != 3 || f.wire != 2) continue;
        const uint8_t *uri;
        size_t uri_len;
        if (pb_find_bytes(f.data, f.len, 1, &uri, &uri_len) == 0 &&
            track_uri_to_id(uri, uri_len, out[count].id) == 0)
            ++count;
    }
    return count;
}

/* Album { cover_group = 17 { image = 1 { file_id = 1, size = 2 } } }, with
 * the older repeated cover = 9 as fallback. Prefers SMALL (size 1). */
static void parse_album_cover(const uint8_t *album, size_t len, spotify_track *t)
{
    const uint8_t *group;
    size_t group_len;
    const uint8_t *images = album;
    size_t images_len = len;
    uint32_t image_field = 9;
    if (pb_find_bytes(album, len, 17, &group, &group_len) == 0) {
        images = group;
        images_len = group_len;
        image_field = 1;
    }
    pb_reader r;
    pb_field f;
    int best = -1;
    pb_init(&r, images, images_len);
    while (pb_next(&r, &f) == 1) {
        if (f.field != image_field || f.wire != 2) continue;
        const uint8_t *id = NULL;
        int size = 0;
        pb_reader ir;
        pb_field iff;
        pb_init(&ir, f.data, f.len);
        while (pb_next(&ir, &iff) == 1) {
            if (iff.field == 1 && iff.wire == 2 && iff.len == 20) id = iff.data;
            if (iff.field == 2 && iff.wire == 0) size = (int)iff.varint;
        }
        /* rank: SMALL (1) best, then DEFAULT (0), then larger ones */
        int rank = size == 1 ? 3 : size == 0 ? 2 : 1;
        if (id && rank > best) {
            memcpy(t->cover, id, 20);
            t->has_cover = 1;
            best = rank;
        }
    }
}

/* Track { name = 2, album = 3, artist = 4 { name = 2 }, duration = 7 (sint32) } */
static void parse_track_proto(const uint8_t *track, size_t len, spotify_track *t)
{
    pb_reader r;
    pb_field f;
    int have_artist = 0;
    pb_init(&r, track, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field == 2 && f.wire == 2) {
            copy_str(t->name, sizeof t->name, f.data, f.len);
        } else if (f.field == 3 && f.wire == 2) {
            parse_album_cover(f.data, f.len, t);
        } else if (f.field == 4 && f.wire == 2 && !have_artist) {
            const uint8_t *name;
            size_t name_len;
            if (pb_find_bytes(f.data, f.len, 2, &name, &name_len) == 0) {
                copy_str(t->artist, sizeof t->artist, name, name_len);
                have_artist = 1;
            }
        } else if (f.field == 7 && f.wire == 0) {
            int64_t ms = (int64_t)(f.varint >> 1) ^ -(int64_t)(f.varint & 1);
            t->duration_ms = ms > 0 ? (unsigned int)ms : 0;
        }
    }
}

static int sp_details_unlocked(const char *client_token, const char *access_token,
                               spotify_track *tracks, int count)
{
    static uint8_t body[SPOTIFY_DETAILS_BATCH * 64];
    if (count > SPOTIFY_DETAILS_BATCH) count = SPOTIFY_DETAILS_BATCH;
    if (sp_headers(client_token, access_token, "application/x-protobuf") < 0) return -1;
    /* BatchedEntityRequest { entity_request = 2 { entity_uri = 1,
     *   query = 2 { extension_kind = 1 (TRACK_V4 = 10) } } } */
    buf_writer w = { body, sizeof body, 0 };
    for (int i = 0; i < count; ++i) {
        uint8_t entity[64], query[4];
        char uri[40];
        snprintf(uri, sizeof uri, "spotify:track:%s", tracks[i].id);
        buf_writer qw = { query, sizeof query, 0 };
        bw_put_varint_field(&qw, 1, 10);
        buf_writer ew = { entity, sizeof entity, 0 };
        bw_put_string(&ew, 1, uri);
        bw_put_bytes(&ew, 2, query, qw.len);
        bw_put_bytes(&w, 2, entity, ew.len);
        if (!tracks[i].name[0]) snprintf(tracks[i].name, sizeof tracks[i].name, "%s", tracks[i].id);
    }
    size_t len = 0;
    int status = request_all("POST", SPCLIENT "/extended-metadata/v0/extended-metadata",
                             headers, body, w.len, &len);
    if (status != 200) {
        log_printf("LIBRARY: details HTTP %d\n", status);
        return status < 0 ? status : -status;
    }
    /* extended_metadata(2) -> extension_data(3) { entity_uri = 2,
     * extension_data = 3 (Any { value = 2 } = Track) } per track */
    int filled = 0;
    pb_reader r;
    pb_field f;
    pb_init(&r, resp_buf, len);
    while (pb_next(&r, &f) == 1) {
        if (f.field != 2 || f.wire != 2) continue;
        pb_reader ar;
        pb_field af;
        pb_init(&ar, f.data, f.len);
        while (pb_next(&ar, &af) == 1) {
            if (af.field != 3 || af.wire != 2) continue;
            const uint8_t *uri, *any, *track;
            size_t uri_len, any_len, track_len;
            char id[24];
            if (pb_find_bytes(af.data, af.len, 2, &uri, &uri_len) < 0 ||
                track_uri_to_id(uri, uri_len, id) < 0 ||
                pb_find_bytes(af.data, af.len, 3, &any, &any_len) < 0 ||
                pb_find_bytes(any, any_len, 2, &track, &track_len) < 0)
                continue;
            for (int i = 0; i < count; ++i)
                if (strcmp(tracks[i].id, id) == 0) {
                    parse_track_proto(track, track_len, &tracks[i]);
                    ++filled;
                    break;
                }
        }
    }
    return filled;
}

/* ---------------------------------------------------------------- locking */

static SceUID api_lock = -1;

void spotify_webapi_init(void)
{
    if (api_lock < 0) api_lock = sceKernelCreateSema("webapi", 0, 1, 1, NULL);
}

static void lock(void) { sceKernelWaitSema(api_lock, 1, NULL); }
static void unlock(void) { sceKernelSignalSema(api_lock, 1); }

int spotify_sp_playlists(const char *client_token, const char *access_token,
                         const char *username, spotify_playlist *out, int max)
{
    lock();
    int r = sp_playlists_unlocked(client_token, access_token, username, out, max);
    unlock();
    return r;
}

int spotify_sp_tracks(const char *client_token, const char *access_token,
                      const char *username, const char *playlist_id,
                      spotify_track *out, int max)
{
    lock();
    int r = sp_tracks_unlocked(client_token, access_token, username, playlist_id, out, max);
    unlock();
    return r;
}

int spotify_sp_track_details(const char *client_token, const char *access_token,
                             spotify_track *tracks, int count)
{
    lock();
    int r = sp_details_unlocked(client_token, access_token, tracks, count);
    unlock();
    return r;
}

int spotify_client_token(const char *client_id, char *out, size_t cap)
{
    lock();
    int r = spotify_client_token_unlocked(client_id, out, cap);
    unlock();
    return r;
}

int spotify_oauth_device_pair(char *access_token, size_t access_cap,
                              char *refresh_token, size_t refresh_cap,
                              void (*show_code)(const char *user_code))
{
    lock();
    int r = spotify_oauth_device_pair_unlocked(access_token, access_cap, refresh_token,
                                               refresh_cap, show_code);
    unlock();
    return r;
}

int spotify_oauth_refresh(const char *refresh_token, char *access_token, size_t access_cap,
                          char *new_refresh, size_t refresh_cap)
{
    lock();
    int r = spotify_oauth_refresh_unlocked(refresh_token, access_token, access_cap, new_refresh, refresh_cap);
    unlock();
    return r;
}

int spotify_login5(const char *client_id, const char *client_token, const char *username,
                   const uint8_t *stored_credential, size_t stored_credential_len,
                   char *access_token, size_t cap)
{
    lock();
    int r = spotify_login5_unlocked(client_id, client_token, username, stored_credential, stored_credential_len, access_token, cap);
    unlock();
    return r;
}

int spotify_track_files(const char *client_token, const char *access_token,
                        const char *track_id, spotify_audio_file *files, int max_files)
{
    lock();
    int r = spotify_track_files_unlocked(client_token, access_token, track_id, files, max_files);
    unlock();
    return r;
}

int spotify_storage_resolve(const char *client_token, const char *access_token,
                            const uint8_t file_id[SPOTIFY_FILE_ID_LEN],
                            char *cdn_urls, size_t url_cap, int max_urls)
{
    lock();
    int r = spotify_storage_resolve_unlocked(client_token, access_token, file_id, cdn_urls, url_cap, max_urls);
    unlock();
    return r;
}
