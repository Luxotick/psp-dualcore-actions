#ifndef SPOTIFY_STREAM_H
#define SPOTIFY_STREAM_H

#include <stdint.h>
#include <stddef.h>

int spotify_stream_download(const char *hostname, const char *url_path, const char *save_path);

#endif /* SPOTIFY_STREAM_H */
