#ifndef SPOTIFY_LOG_H
#define SPOTIFY_LOG_H

/* Prints to the debug screen and appends the same text to spotify.log in
 * the application directory, so a run can be read back over USB. */

/* dir: the application directory. The log path is absolute because the
 * current directory set with sceIoChdir does not carry over to threads. */
void log_init(const char *dir);
int log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 0: file only (the UI owns the screen); 1: also the debug screen. */
void log_set_echo(int on);

#endif /* SPOTIFY_LOG_H */
