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
    return 0;
}

static int renew_tokens(spotify_ctx *c)
{
    log_printf("SESSION: renewing tokens\n");
    if (c->cfg.refresh_token[0]) oauth(c, NULL);
    return bearer_tokens(c);
}

int spotify_ctx_play(spotify_ctx *c, const char *track_id, player_control *control)
{
    static spotify_audio_file files[16];
    static char cdn_urls[4][1024];

    if (sceKernelGetSystemTimeWide() - c->tokens_at > TOKEN_RENEW_US) renew_tokens(c);

    log_printf("SESSION: track %s\n", track_id);
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

    uint8_t gid[SPOTIFY_GID_LEN], key[16];
    if (spotify_base62_to_gid(track_id, gid) < 0) return -3;
    /* The AP session idles while tracks play; reconnect once if it died. */
    int ret = c->ap_connected ? spotify_request_audio_key(&c->ap, file->file_id, gid, key) : -1;
    if (ret < 0) {
        log_printf("SESSION: audio key failed %d, reconnecting AP\n", ret);
        if (ap_login(c) < 0) return -4;
        ret = spotify_request_audio_key(&c->ap, file->file_id, gid, key);
        if (ret < 0) return ret;
    }

    int urls = spotify_storage_resolve(c->client_token, c->access_token, file->file_id,
                                       cdn_urls[0], sizeof cdn_urls[0],
                                       (int)(sizeof cdn_urls / sizeof cdn_urls[0]));
    if (urls <= 0) return urls < 0 ? urls : -5;
    return spotify_play_ogg(cdn_urls[0], sizeof cdn_urls[0], urls, key, control);
}

void spotify_ctx_stop(spotify_ctx *c)
{
    if (c->ap_connected) spotify_disconnect(&c->ap);
    c->ap_connected = 0;
}
