#include "ui.h"
#include "gfx.h"
#include "log.h"
#include "player.h"
#include "session.h"
#include "webapi.h"
#include <stdio.h>
#include <string.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspthreadman.h>
#include <stdlib.h>
#include "http.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define MAX_PLAYLISTS 100
#define MAX_TRACKS 500
#define VISIBLE_ROWS 9
#define ROW_HEIGHT 14
#define TEXT_COLS 55

typedef enum { VIEW_PLAYLISTS, VIEW_TRACKS } view_t;
typedef enum { PS_IDLE, PS_LOADING, PS_PLAYING, PS_ERROR } play_state_t;

static spotify_ctx ctx;

/* Library browsing (UI thread only). Entry 0 is Liked Songs. */
static spotify_playlist playlists[MAX_PLAYLISTS + 1];
static int playlist_count;
static spotify_track tracks[MAX_TRACKS];
static int track_count;
static int open_playlist = -1;   /* index into playlists of `tracks` */
static view_t view = VIEW_PLAYLISTS;
static int selected, scroll;

/* Play queue: a copy of the list a track was started from, so browsing
 * other playlists does not change what plays next. */
static spotify_track queue[MAX_TRACKS];
static volatile int queue_len;
static int queue_playlist = -1;

/* Shared with the player thread. */
static player_control control;
static volatile int request_index = -1;
static volatile int current_index = -1;
static volatile play_state_t play_state = PS_IDLE;
static volatile int player_quit;
static volatile int me_dead;
static int volume_pct = 80;

/* Album cover of the playing track (UI thread only). */
#define COVER_SIZE 64
static unsigned int cover_px[COVER_SIZE * COVER_SIZE];
static int cover_ok;
static char cover_track[24];

/* ------------------------------------------------------------ helpers */

static void format_time(unsigned int ms, char *out, size_t cap)
{
    unsigned int s = ms / 1000u;
    snprintf(out, cap, "%u:%02u", s / 60u, s % 60u);
}

static void title_bar(void)
{
    gfx_rect(0, 0, GFX_WIDTH, 24, GFX_GREEN);
    gfx_text("PSPotify", 8, 8, GFX_BLACK, 0);
    gfx_text("native Spotify on PSP / Media Engine decode", 112, 8, GFX_BLACK, 0);
}

/* Centered single-message screen used while signing in. */
static void message_screen(const char *line1, const char *line2)
{
    gfx_clear(GFX_BLACK);
    title_bar();
    gfx_text(line1, (GFX_WIDTH - (int)strlen(line1) * 8) / 2, 120, GFX_WHITE, TEXT_COLS);
    if (line2) gfx_text(line2, (GFX_WIDTH - (int)strlen(line2) * 8) / 2, 140, GFX_LIGHT_GRAY, TEXT_COLS);
    gfx_flip();
    sceDisplayWaitVblankStart();
}

static void on_status(const char *message)
{
    message_screen(message, NULL);
}

static void on_pair_code(const char *code)
{
    gfx_clear(GFX_BLACK);
    title_bar();
    gfx_text("Pair this PSP with your Spotify account", 80, 60, GFX_WHITE, 0);
    gfx_text("On your phone or computer open", 120, 84, GFX_LIGHT_GRAY, 0);
    gfx_text("spotify.com/pair", 176, 100, GFX_GREEN, 0);
    gfx_text("and enter this code:", 160, 116, GFX_LIGHT_GRAY, 0);
    int w = (int)strlen(code) * 8 * 4;
    gfx_rect((GFX_WIDTH - w) / 2 - 12, 140, w + 24, 48, GFX_DARK_GRAY);
    gfx_text_big(code, (GFX_WIDTH - w) / 2, 148, GFX_WHITE, 4);
    gfx_text("Waiting for approval... (one time only)", 84, 212, GFX_LIGHT_GRAY, 0);
    gfx_flip();
    sceDisplayWaitVblankStart();
}

/* ------------------------------------------------------------ player thread */

static int player_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int failures = 0;
    while (!player_quit) {
        int idx = request_index;
        if (idx < 0 || idx >= queue_len) {
            sceKernelDelayThread(20000);
            continue;
        }
        request_index = -1;
        current_index = idx;
        control.stop = 0;
        control.paused = 0;
        control.position_ms = 0;
        control.fetched_pct = 0;
        play_state = PS_LOADING;

        int ret = spotify_ctx_play(&ctx, queue[idx].id, &control);
        log_printf("UI: track %d ended (%d)\n", idx, ret);
        if (ret == PLAYER_ERR_ME_TIMEOUT) {
            me_dead = 1;
            play_state = PS_ERROR;
            break;
        }
        if (player_quit) break;
        if (request_index >= 0) { failures = 0; continue; }   /* user picked another */
        if (ret == PLAYER_STOPPED) { play_state = PS_IDLE; continue; }
        if (ret < 0) {
            /* Unplayable track: skip ahead, but give up after a few in a row. */
            play_state = PS_ERROR;
            if (++failures >= 3) continue;
            sceKernelDelayThread(500000);
        } else {
            failures = 0;
        }
        if (idx + 1 < queue_len) request_index = idx + 1;
        else play_state = PS_IDLE;
    }
    return 0;
}

static void start_track(int queue_idx)
{
    if (queue_idx < 0 || queue_idx >= queue_len) return;
    request_index = queue_idx;
    control.stop = 1;           /* ends the current track, if any */
}

/* ------------------------------------------------------------ data loading */

static void load_playlists(void)
{
    message_screen("Loading your library...", NULL);
    memset(&playlists[0], 0, sizeof playlists[0]);
    snprintf(playlists[0].name, sizeof playlists[0].name, "Liked Songs");
    playlists[0].total = -1;
    int n = spotify_sp_playlists(ctx.client_token, ctx.access_token, ctx.cfg.username,
                                 &playlists[1], MAX_PLAYLISTS);
    playlist_count = 1 + (n > 0 ? n : 0);
    log_printf("UI: %d playlists\n", n);
}

static void load_tracks(int playlist)
{
    message_screen("Loading tracks...", playlists[playlist].name);
    memset(tracks, 0, sizeof tracks);
    int n = spotify_sp_tracks(ctx.client_token, ctx.access_token, ctx.cfg.username,
                              playlists[playlist].id, tracks, MAX_TRACKS);
    track_count = n > 0 ? n : 0;
    /* Names come from batched metadata lookups; show progress meanwhile. */
    for (int i = 0; i < track_count; i += SPOTIFY_DETAILS_BATCH) {
        char line[48];
        snprintf(line, sizeof line, "Loading track names %d/%d", i, track_count);
        message_screen(line, playlists[playlist].name);
        int batch = track_count - i < SPOTIFY_DETAILS_BATCH ? track_count - i
                                                             : SPOTIFY_DETAILS_BATCH;
        spotify_sp_track_details(ctx.client_token, ctx.access_token, &tracks[i], batch);
    }
    open_playlist = playlist;
    if (playlist == 0) playlists[0].total = track_count;
    log_printf("UI: %d tracks in list %d\n", n, playlist);
}

/* ------------------------------------------------------------ cover art */

/* Downloads and decodes the playing track's cover into cover_px (blocking,
 * a few KB from i.scdn.co). Called when the playing track changes. */
static void load_cover(const spotify_track *t)
{
    static uint8_t jpeg[128 * 1024];
    cover_ok = 0;
    snprintf(cover_track, sizeof cover_track, "%s", t->id);
    if (!t->has_cover) return;

    char url[80];
    int n = snprintf(url, sizeof url, "https://i.scdn.co/image/");
    for (int i = 0; i < 20 && n > 0 && (size_t)n + 3 < sizeof url; ++i)
        n += snprintf(url + n, sizeof url - (size_t)n, "%02x", t->cover[i]);

    http_stream s;
    uint64_t content_length = 0;
    int status = http_stream_open(&s, url, &content_length);
    if (status != 200) {
        if (status > 0) http_stream_close(&s);
        log_printf("UI: cover HTTP %d\n", status);
        return;
    }
    size_t got = 0;
    for (;;) {
        int r = http_stream_read(&s, jpeg + got, (unsigned int)(sizeof jpeg - got));
        if (r <= 0) break;
        got += (size_t)r;
        if (got == sizeof jpeg) break;
    }
    http_stream_close(&s);

    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load_from_memory(jpeg, (int)got, &w, &h, &comp, 4);
    if (!rgba) {
        log_printf("UI: cover decode failed (%u bytes)\n", (unsigned int)got);
        return;
    }
    /* Nearest-neighbour scale to COVER_SIZE, RGBA -> ABGR8888 framebuffer. */
    for (int y = 0; y < COVER_SIZE; ++y)
        for (int x = 0; x < COVER_SIZE; ++x) {
            const unsigned char *p = rgba + 4 * ((y * h / COVER_SIZE) * w + x * w / COVER_SIZE);
            cover_px[y * COVER_SIZE + x] = 0xFF000000u | ((unsigned int)p[2] << 16) |
                                           ((unsigned int)p[1] << 8) | p[0];
        }
    stbi_image_free(rgba);
    cover_ok = 1;
    log_printf("UI: cover %dx%d\n", w, h);
}

/* ------------------------------------------------------------ rendering */

static void draw_now_playing(void)
{
    const int y = 28;
    gfx_rect(0, y, GFX_WIDTH, 72, GFX_DARK_GRAY);
    int idx = current_index;
    if (cover_ok && idx >= 0 && idx < queue_len && strcmp(cover_track, queue[idx].id) == 0) {
        gfx_image(4, y + 4, COVER_SIZE, COVER_SIZE, cover_px);
    } else {
        gfx_rect(4, y + 4, COVER_SIZE, COVER_SIZE, GFX_MID_GRAY);
        gfx_text("ME", 28, y + 32, GFX_GREEN, 0);
    }

    const int x = 76;
    play_state_t st = play_state;
    if (idx < 0 || idx >= queue_len) {
        gfx_text("Nothing playing", x, y + 10, GFX_LIGHT_GRAY, 0);
        gfx_text("Pick a playlist and press X on a song", x, y + 26, GFX_LIGHT_GRAY, 0);
    } else {
        const spotify_track *t = &queue[idx];
        /* Stop before the status column at x = 340. */
        gfx_text(t->name, x, y + 6, GFX_GREEN, (340 - 8 - x) / 8);
        gfx_text(t->artist, x, y + 20, GFX_LIGHT_GRAY, (340 - 8 - x) / 8);
        const char *label = st == PS_LOADING && control.position_ms == 0 ? "BUFFERING"
                          : st == PS_ERROR ? "ERROR"
                          : control.paused ? "PAUSED"
                          : st == PS_IDLE ? "STOPPED" : "PLAYING";
        char buf[48];
        snprintf(buf, sizeof buf, "%s  Vol %d%%", label, volume_pct);
        gfx_text(buf, 340, y + 6, GFX_WHITE, 17);
        snprintf(buf, sizeof buf, "%d/%d", idx + 1, queue_len);
        gfx_text(buf, 340, y + 20, GFX_LIGHT_GRAY, 17);

        unsigned int pos = control.position_ms, dur = t->duration_ms;
        char tpos[12], tdur[12];
        format_time(pos, tpos, sizeof tpos);
        format_time(dur, tdur, sizeof tdur);
        const int bar_x = x + 48, bar_w = 300, bar_y = y + 46;
        gfx_text(tpos, x, bar_y - 2, GFX_WHITE, 6);
        gfx_rect(bar_x, bar_y, bar_w, 5, GFX_MID_GRAY);
        unsigned int fetched = control.fetched_pct;
        gfx_rect(bar_x, bar_y, (int)(bar_w * (fetched > 100 ? 100 : fetched) / 100), 5, 0xFF606060u);
        if (dur > 0) {
            unsigned int fill = (unsigned int)((unsigned long long)pos * bar_w / dur);
            gfx_rect(bar_x, bar_y, (int)(fill > (unsigned int)bar_w ? (unsigned int)bar_w : fill),
                     5, GFX_GREEN);
        }
        gfx_text(tdur, bar_x + bar_w + 8, bar_y - 2, GFX_WHITE, 6);
    }
}

static void draw_list(void)
{
    char buf[192];
    const int header_y = 106;
    int count = view == VIEW_PLAYLISTS ? playlist_count : track_count;
    if (view == VIEW_PLAYLISTS)
        snprintf(buf, sizeof buf, "Your Library (%d)", playlist_count);
    else
        snprintf(buf, sizeof buf, "%s (%d songs)", playlists[open_playlist].name, track_count);
    gfx_text(buf, 10, header_y, GFX_WHITE, TEXT_COLS);

    int y = header_y + 16;
    if (scroll > 0) gfx_text("^", 466, y, GFX_LIGHT_GRAY, 0);
    for (int i = scroll; i < count && i < scroll + VISIBLE_ROWS; ++i) {
        int sel = i == selected;
        if (sel) gfx_rect(5, y - 3, 470, ROW_HEIGHT, GFX_LIGHT_GRAY);
        unsigned int color = sel ? GFX_BLACK : GFX_WHITE;
        if (view == VIEW_PLAYLISTS) {
            const spotify_playlist *p = &playlists[i];
            if (p->total >= 0) snprintf(buf, sizeof buf, "%s  (%d)", p->name, p->total);
            else snprintf(buf, sizeof buf, "%s", p->name);
        } else {
            const spotify_track *t = &tracks[i];
            snprintf(buf, sizeof buf, "%d. %s - %s", i + 1, t->name, t->artist);
            int playing = queue_playlist == open_playlist && current_index >= 0 &&
                          strcmp(queue[current_index].id, t->id) == 0;
            if (playing) gfx_text(">", 8, y, sel ? GFX_BLACK : GFX_GREEN, 0);
        }
        gfx_text(buf, 20, y, color, TEXT_COLS);
        y += ROW_HEIGHT;
    }
    if (scroll + VISIBLE_ROWS < count) gfx_text("v", 466, y - ROW_HEIGHT, GFX_LIGHT_GRAY, 0);
    if (count == 0) gfx_text("(empty)", 20, y, GFX_LIGHT_GRAY, 0);
}

static void draw(void)
{
    gfx_clear(GFX_BLACK);
    title_bar();
    draw_now_playing();
    draw_list();
    gfx_rect(0, 256, GFX_WIDTH, 16, GFX_DARK_GRAY);
    gfx_text("X:Play O:Back []:Pause L/R:Prev/Next <>:Vol START:Exit", 8, 260, GFX_WHITE, 0);
    gfx_flip();
}

/* ------------------------------------------------------------ input */

static void move_selection(int delta, int count)
{
    if (count <= 0) return;
    selected += delta;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + VISIBLE_ROWS) scroll = selected - VISIBLE_ROWS + 1;
}

static void set_volume(int pct)
{
    volume_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    control.volume = PSP_AUDIO_VOLUME_MAX * volume_pct / 100;
}

int spotify_ui_run(volatile int *exit_requested)
{
    log_set_echo(0);   /* the debug console would scribble over our frames */
    gfx_init();
    spotify_ui_hooks hooks = { on_status, on_pair_code };
    int ret = spotify_ctx_start(&ctx, &hooks);
    if (ret < 0) {
        message_screen("Could not sign in to Spotify", "See spotify.log. START: exit");
        SceCtrlData pad;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThreadCB(50000);
        } while (!(pad.Buttons & PSP_CTRL_START) && !*exit_requested);
        return 0;
    }
    set_volume(volume_pct);
    load_playlists();

    SceUID thid = sceKernelCreateThread("player", player_thread, 0x1C, 0x20000, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);

    unsigned int previous = 0, held_frames = 0, frame = 0;
    int dirty = 1;
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~previous;
        /* Auto-repeat for up/down while held. */
        unsigned int dirs = pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN);
        held_frames = (dirs && dirs == (previous & dirs)) ? held_frames + 1 : 0;
        if (held_frames > 18 && held_frames % 4 == 0) pressed |= dirs;
        previous = pad.Buttons;

        if ((pressed & PSP_CTRL_START) || *exit_requested) break;
        int count = view == VIEW_PLAYLISTS ? playlist_count : track_count;
        if (pressed & PSP_CTRL_UP) { move_selection(-1, count); dirty = 1; }
        if (pressed & PSP_CTRL_DOWN) { move_selection(1, count); dirty = 1; }
        if (pressed & PSP_CTRL_LEFT) { set_volume(volume_pct - 10); dirty = 1; }
        if (pressed & PSP_CTRL_RIGHT) { set_volume(volume_pct + 10); dirty = 1; }
        if (pressed & PSP_CTRL_SQUARE) { control.paused = !control.paused; dirty = 1; }
        if (pressed & PSP_CTRL_RTRIGGER) { start_track(current_index + 1); dirty = 1; }
        if (pressed & PSP_CTRL_LTRIGGER) {
            /* Restart the song if it has been playing a while, else go back. */
            start_track(control.position_ms > 3000 ? current_index : current_index - 1);
            dirty = 1;
        }
        if ((pressed & PSP_CTRL_CIRCLE) && view == VIEW_TRACKS) {
            view = VIEW_PLAYLISTS;
            selected = open_playlist;
            scroll = selected >= VISIBLE_ROWS ? selected - VISIBLE_ROWS + 1 : 0;
            dirty = 1;
        }
        if ((pressed & PSP_CTRL_CROSS) && count > 0) {
            if (view == VIEW_PLAYLISTS) {
                load_tracks(selected);
                view = VIEW_TRACKS;
                selected = scroll = 0;
            } else {
                /* Queue this list unless it is already the queue. */
                if (queue_playlist != open_playlist || queue_len != track_count) {
                    control.stop = 1;
                    request_index = -1;
                    while (play_state == PS_LOADING && current_index >= 0 && !me_dead)
                        sceKernelDelayThread(10000);
                    memcpy(queue, tracks, sizeof tracks[0] * (size_t)track_count);
                    queue_len = track_count;
                    queue_playlist = open_playlist;
                }
                start_track(selected);
            }
            dirty = 1;
        }

        /* New track: fetch its cover (blocks this thread briefly). */
        int playing = current_index;
        if (playing >= 0 && playing < queue_len && strcmp(cover_track, queue[playing].id) != 0) {
            load_cover(&queue[playing]);
            dirty = 1;
        }

        /* Progress moves on its own: repaint a few times per second. */
        if (dirty || frame % 15 == 0) {
            draw();
            dirty = 0;
        }
        ++frame;
        sceDisplayWaitVblankStartCB();
    }

    message_screen("Stopping...", NULL);
    player_quit = 1;
    control.stop = 1;
    if (thid >= 0) {
        SceUInt timeout = 25u * 1000u * 1000u;
        if (sceKernelWaitThreadEnd(thid, &timeout) < 0) sceKernelTerminateThread(thid);
        sceKernelDeleteThread(thid);
    }
    spotify_ctx_stop(&ctx);
    return me_dead ? PLAYER_ERR_ME_TIMEOUT : 0;
}
