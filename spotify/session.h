#ifndef SPOTIFY_SESSION_H
#define SPOTIFY_SESSION_H

#include <pspkerneltypes.h>
#include "config.h"
#include "handshake.h"
#include "player.h"

/* Everything needed to browse and play: AP session (audio keys), a
 * login5 Bearer token for spclient, the OAuth token for the Web API. */
typedef struct {
    spotify_config cfg;
    spotify_session ap;
    int ap_connected;
    char client_token[512];
    char access_token[SPOTIFY_CFG_MAX_STR];   /* login5: spclient */
    char web_token[SPOTIFY_CFG_MAX_STR];      /* OAuth: api.spotify.com */
    SceInt64 tokens_at;
    /* Background AP keepalive (pings) and next-track prefetch. */
    SceUID keepalive_thread;
    volatile int keepalive_quit;
} spotify_ctx;

/* Progress for the UI while starting: either a status line or, during first
 * time pairing, the code to enter at spotify.com/pair. */
typedef struct {
    void (*status)(const char *message);
    void (*pair_code)(const char *code);
} spotify_ui_hooks;

/* Pairing/refresh, AP login, client token and login5. Returns 0 when the
 * Web API and playback are usable. */
int spotify_ctx_start(spotify_ctx *c, const spotify_ui_hooks *ui);

/* Resolves and plays one track (blocking; stop it via control->stop).
 * Returns the spotify_play_ogg result or another negative error. */
int spotify_ctx_play(spotify_ctx *c, const char *track_id, player_control *control);

/* Resolve this track (metadata, audio key, CDN URLs) in the background a few
 * seconds from now, so starting it later skips straight to the download. */
void spotify_ctx_prefetch(spotify_ctx *c, const char *track_id);

void spotify_ctx_stop(spotify_ctx *c);

#endif /* SPOTIFY_SESSION_H */
