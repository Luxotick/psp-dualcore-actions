#ifndef SPOTIFY_MERCURY_H
#define SPOTIFY_MERCURY_H

#include "handshake.h"
#include <stddef.h>
#include <stdint.h>

/* Mercury GET over the AP connection (MercuryReq 0xb2). Copies the first
 * payload part into out. Returns the Mercury status code (e.g. 200) or a
 * negative error. */
int spotify_mercury_get(spotify_session *session, const char *uri,
                        uint8_t *out, size_t cap, size_t *out_len);

/* Access token from hm://keymaster/token/authenticated (JSON accessToken). */
int spotify_keymaster_token(spotify_session *session, const char *client_id,
                            const char *scopes, char *token, size_t cap);

#endif /* SPOTIFY_MERCURY_H */
