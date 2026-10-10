#ifndef SPOTIFY_UI_H
#define SPOTIFY_UI_H

/* Full-screen Spotify browser/player (layout and controls follow PSPotify).
 * Signs in (pairing on first run), lists Liked Songs and the user's
 * playlists, and plays tracks on a player thread with Vorbis decoded on the
 * Media Engine. Returns when the user exits or *exit_requested is set.
 * Returns PLAYER_ERR_ME_TIMEOUT if the ME stopped responding (reboot
 * needed to exit), otherwise 0. */
int spotify_ui_run(volatile int *exit_requested);

#endif /* SPOTIFY_UI_H */
