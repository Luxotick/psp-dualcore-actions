#include "session.h"
#include "audiokey.h"
#include "log.h"
#include "login.h"
#include "webapi.h"
#include <stdio.h>
#include <string.h>
#include <pspthreadman.h>

/* Both Bearer tokens live 3600 s; renew well before that. */
#define TOKEN_RENEW_US (50ll * 60 * 1000 * 1000)

static int keepalive_thread(SceSize args, void *argp);
/* AP access (audio keys vs keepalive) and track resolution locks. */
static SceUID ap_lock = -1;
static SceUID resolve_lock = -1;

static void say(const spotify_ui_hooks *ui, const char *message)
{
    log_printf("SESSION: %s\n", message);
    if (ui && ui->status) ui->status(message);
}

/* Keymaster-client OAuth: refresh token if we have one, otherwise device
 * pairing at spotify.com/pair. The access token serves the Web API and the
 * first AP login, which makes the stored blob a keymaster-client blob that
 * login5 accepts (a blob from a web player token is rejected). */
static int oauth(spotify_ctx *c, const spotify_ui_hooks *ui)
{
    static char refresh[SPOTIFY_CFG_MAX_STR];
    int ret;
    refresh[0] = '\0';
    if (c->cfg.refresh_token[0]) {
        ret = spotify_oauth_refresh(c->cfg.refresh_token, c->web_token, sizeof c->web_token,
                                    refresh, sizeof refresh);
    } else {
        say(ui, "Pairing with Spotify...");
        ret = spotify_oauth_device_pair(c->web_token, sizeof c->web_token,
                                        refresh, sizeof refresh,
                                        ui ? ui->pair_code : NULL);
    }
    if (ret < 0) {
        c->web_token[0] = '\0';
        return ret;
    }
    if (refresh[0]) {
        snprintf(c->cfg.refresh_token, sizeof c->cfg.refresh_token, "%s", refresh);
        spotify_config_save_blob(&c->cfg, c->cfg.username, c->cfg.blob, c->cfg.blob_len);
    }
    return 0;
}

/* AP connect + login. Uses the fresh OAuth token when the stored blob is not
 * a keymaster-client blob yet, else the blob. Reloads the config afterwards
 * because login stores the new reusable blob on the Memory Stick. */
static int ap_login(spotify_ctx *c)
{
    if (c->ap_connected) {
        spotify_disconnect(&c->ap);
        c->ap_connected = 0;
    }
    int ret = spotify_connect_and_handshake(&c->ap);
    if (ret < 0) return ret;
    int use_token = c->web_token[0] &&
                    (c->cfg.blob_len == 0 ||
                     strcmp(c->cfg.blob_client, SPOTIFY_CLIENT_ID_KEYMASTER) != 0);
    if (use_token) {
        snprintf(c->cfg.token, sizeof c->cfg.token, "%s", c->web_token);
        c->cfg.auth_type = AUTH_TYPE_SPOTIFY_TOKEN;
        /* The blob this login returns belongs to the keymaster client. */
        snprintf(c->cfg.blob_client, sizeof c->cfg.blob_client, "%s",
                 SPOTIFY_CLIENT_ID_KEYMASTER);
    } else {
        c->cfg.auth_type = AUTH_TYPE_STORED_CREDENTIALS;
    }
    ret = spotify_login(&c->ap, &c->cfg);
    if (ret < 0) {
        spotify_disconnect(&c->ap);
        return ret;
    }
    c->ap_connected = 1;
    spotify_config_load(&c->cfg);
    return 0;
}

static int bearer_tokens(spotify_ctx *c)
{
    int ret = spotify_client_token(SPOTIFY_CLIENT_ID_KEYMASTER, c->client_token,
                                   sizeof c->client_token);
    if (ret < 0) return ret;
    ret = spotify_login5(SPOTIFY_CLIENT_ID_KEYMASTER, c->client_token, c->cfg.username,
                         c->cfg.blob, c->cfg.blob_len, c->access_token, sizeof c->access_token);
    if (ret < 0) {
        if (!c->web_token[0]) return ret;
        /* The OAuth token is a keymaster-client Bearer token as well. */
        log_printf("SESSION: login5 failed %d, using OAuth token for spclient\n", ret);
        snprintf(c->access_token, sizeof c->access_token, "%s", c->web_token);
    }
    if (!c->web_token[0])
        snprintf(c->web_token, sizeof c->web_token, "%s", c->access_token);
    c->tokens_at = sceKernelGetSystemTimeWide();
    return 0;
}

int spotify_ctx_start(spotify_ctx *c, const spotify_ui_hooks *ui)
{
    memset(c, 0, sizeof *c);
    c->keepalive_thread = -1;
    spotify_webapi_init();
    spotify_config_load(&c->cfg);   /* missing file is fine: pairing creates it */

    say(ui, "Signing in...");
    int ret = oauth(c, ui);
    if (ret < 0 && c->cfg.blob_len == 0) {
        say(ui, "Sign-in failed (no stored credentials)");
        return ret;
    }
    say(ui, "Connecting to Spotify...");
    if ((ret = ap_login(c)) < 0) {
        say(ui, "Spotify login failed");
        return ret;
    }
    if ((ret = bearer_tokens(c)) < 0) {
        say(ui, "Could not get an access token");
        return ret;
    }
    if (ap_lock < 0) ap_lock = sceKernelCreateSema("ap", 0, 1, 1, NULL);
    if (resolve_lock < 0) resolve_lock = sceKernelCreateSema("resolve", 0, 1, 1, NULL);
    static spotify_ctx *self;
    self = c;
    c->keepalive_thread = sceKernelCreateThread("ap_keepalive", keepalive_thread, 0x20,
                                                0x10000, 0, NULL);
    if (c->keepalive_thread >= 0) sceKernelStartThread(c->keepalive_thread, sizeof self, &self);
    return 0;
}

static int renew_tokens(spotify_ctx *c)
{
    log_printf("SESSION: renewing tokens\n");
    if (c->cfg.refresh_token[0]) oauth(c, NULL);
    return bearer_tokens(c);
}

/* ---- AP access: audio key requests and the keepalive share one lock ---- */


static void ap_acquire(void) { sceKernelWaitSema(ap_lock, 1, NULL); }
static void ap_release(void) { sceKernelSignalSema(ap_lock, 1); }

#define PACKET_PING 0x04
#define PACKET_PONG 0x49

/* Answers AP pings while tracks play; an unanswered AP drops the
 * connection and the next audio key request would hang. */
static void service_ap(spotify_ctx *c)
{
    static uint8_t rx[4096];
    ap_acquire();
    while (c->ap_connected) {
        int ready = spotify_poll_readable(&c->ap, 0);
        if (ready == 0) break;
        uint8_t cmd = 0;
        uint16_t len = 0;
        if (ready < 0 || spotify_recv_packet(&c->ap, &cmd, rx, sizeof rx, &len) < 0) {
            log_printf("SESSION: AP connection lost, will reconnect on demand\n");
            spotify_disconnect(&c->ap);
            c->ap_connected = 0;
            break;
        }
        if (cmd == PACKET_PING) spotify_send_packet(&c->ap, PACKET_PONG, rx, len);
    }
    ap_release();
}

/* ---- track resolution ---- */

typedef struct {
    char id[24];
    uint8_t key[16];
    char cdn_urls[4][1024];
    int url_count;
} resolved_track;

static resolved_track prefetched;      /* valid when prefetched.id[0] */
static char prefetch_id[24];
static SceInt64 prefetch_at;

/* Metadata -> Ogg Vorbis file -> audio key -> CDN URLs. Serialised by
 * resolve_lock (the player and the prefetcher both call it). */
static int resolve_track(spotify_ctx *c, const char *track_id, resolved_track *out)
{
    static spotify_audio_file files[16];
    if (sceKernelGetSystemTimeWide() - c->tokens_at > TOKEN_RENEW_US) renew_tokens(c);

    int count = spotify_track_files(c->client_token, c->access_token, track_id, files,
                                    (int)(sizeof files / sizeof files[0]));
    if (count == -401 && renew_tokens(c) == 0)
        count = spotify_track_files(c->client_token, c->access_token, track_id, files,
                                    (int)(sizeof files / sizeof files[0]));
    if (count <= 0) return count < 0 ? count : -1;

    static const int preference[] = {
        SPOTIFY_FMT_OGG_VORBIS_160, SPOTIFY_FMT_OGG_VORBIS_320, SPOTIFY_FMT_OGG_VORBIS_96
    };
    const spotify_audio_file *file = NULL;
    for (size_t p = 0; p < sizeof preference / sizeof preference[0] && !file; ++p)
        for (int i = 0; i < count; ++i)
            if (files[i].format == preference[p]) { file = &files[i]; break; }
    if (!file) {
        log_printf("SESSION: no Ogg Vorbis file for %s\n", track_id);
        return -2;
    }

    uint8_t gid[SPOTIFY_GID_LEN];
    if (spotify_base62_to_gid(track_id, gid) < 0) return -3;
    ap_acquire();
    int ret = c->ap_connected ? spotify_request_audio_key(&c->ap, file->file_id, gid, out->key) : -1;
    if (ret < 0) {
        log_printf("SESSION: audio key failed %d, reconnecting AP\n", ret);
        ret = ap_login(c);
        if (ret == 0) ret = spotify_request_audio_key(&c->ap, file->file_id, gid, out->key);
    }
    ap_release();
    if (ret < 0) return ret < -4 ? ret : -4;

    out->url_count = spotify_storage_resolve(c->client_token, c->access_token, file->file_id,
                                             out->cdn_urls[0], sizeof out->cdn_urls[0],
                                             (int)(sizeof out->cdn_urls / sizeof out->cdn_urls[0]));
    if (out->url_count <= 0) return out->url_count < 0 ? out->url_count : -5;
    snprintf(out->id, sizeof out->id, "%s", track_id);
    return 0;
}

static int keepalive_thread(SceSize args, void *argp)
{
    (void)args;
    spotify_ctx *c = *(spotify_ctx **)argp;
    static resolved_track scratch;
    while (!c->keepalive_quit) {
        sceKernelDelayThread(500000);
        service_ap(c);
        if (prefetch_id[0] && sceKernelGetSystemTimeWide() >= prefetch_at) {
            char id[24];
            snprintf(id, sizeof id, "%s", prefetch_id);
            prefetch_id[0] = '\0';
            sceKernelWaitSema(resolve_lock, 1, NULL);
            int already = strcmp(prefetched.id, id) == 0;
            int ret = already ? 0 : resolve_track(c, id, &scratch);
            if (!already && ret == 0) prefetched = scratch;
            sceKernelSignalSema(resolve_lock, 1);
            log_printf("SESSION: prefetch %s %s\n", id, ret == 0 ? "ready" : "failed");
        }
    }
    return 0;
}

void spotify_ctx_prefetch(spotify_ctx *c, const char *track_id)
{
    (void)c;
    if (!track_id || !track_id[0]) return;
    /* Leave the current track's own start-up requests a head start. */
    prefetch_at = sceKernelGetSystemTimeWide() + 8ll * 1000 * 1000;
    snprintf(prefetch_id, sizeof prefetch_id, "%s", track_id);
}

int spotify_ctx_play(spotify_ctx *c, const char *track_id, player_control *control)
{
    static resolved_track track;
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    log_printf("SESSION: track %s\n", track_id);

    sceKernelWaitSema(resolve_lock, 1, NULL);
    int ret = 0;
    if (strcmp(prefetched.id, track_id) == 0) {
        track = prefetched;
        prefetched.id[0] = '\0';
    } else {
        ret = resolve_track(c, track_id, &track);
    }
    sceKernelSignalSema(resolve_lock, 1);
    if (ret < 0) return ret;
    log_printf("SESSION: resolved in %u ms\n",
               (unsigned int)((sceKernelGetSystemTimeWide() - t0) / 1000));
    return spotify_play_ogg(track.cdn_urls[0], sizeof track.cdn_urls[0], track.url_count,
                            track.key, control);
}

void spotify_ctx_stop(spotify_ctx *c)
{
    if (c->keepalive_thread >= 0) {
        c->keepalive_quit = 1;
        SceUInt timeout = 15u * 1000u * 1000u;
        if (sceKernelWaitThreadEnd(c->keepalive_thread, &timeout) < 0)
            sceKernelTerminateThread(c->keepalive_thread);
        sceKernelDeleteThread(c->keepalive_thread);
        c->keepalive_thread = -1;
    }
    if (c->ap_connected) spotify_disconnect(&c->ap);
    c->ap_connected = 0;
}
