#ifndef SPOTIFY_LOG_H
#define SPOTIFY_LOG_H

/* Prints to the debug screen and appends the same text to spotify.log in
 * the application directory, so a run can be read back over USB. */

void log_init(void);
int log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* SPOTIFY_LOG_H */
