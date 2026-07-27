#ifndef SGUG_PROTO_OAUTH_H
#define SGUG_PROTO_OAUTH_H

#include "crypto/rsa.h"
#include "net/http.h"
#include "proto/config.h"

#include <stddef.h>

/*
 * Access tokens for the Actions service.
 *
 * The runner authenticates with a private-key JWT rather than a stored secret:
 * each request for a token presents a five minute assertion signed by the key
 * registered at configure time. Tokens themselves last about ten minutes and
 * there is no refresh token, so a new assertion is minted each time.
 */

typedef struct sgug_oauth sgug_oauth;

/* Borrows cfg and key; both must outlive the returned object. */
sgug_oauth *sgug_oauth_new(sgug_http_client *http, const sgug_config *cfg,
    const sgug_rsa *key);
void sgug_oauth_free(sgug_oauth *o);

/*
 * A currently valid bearer token, minted or renewed as required. The returned
 * string is owned by the object and is invalidated by the next call.
 *
 * Renewal happens a minute before expiry rather than on failure, because a
 * token that lapses mid-long-poll costs a wasted 50 second round trip.
 */
const char *sgug_oauth_token(sgug_oauth *o, char *err, size_t errlen);

/*
 * Discards the cached token so the next call mints a fresh one.
 *
 * Call this on a 401 and also on a 400: the service answers an expired
 * assertion with either, and the reference implementations re-authorise on
 * both. Also the correct response to a ForceTokenRefresh message.
 */
void sgug_oauth_invalidate(sgug_oauth *o);

/*
 * True when the service reported invalid_client, which means the runner
 * registration was deleted server side. Retrying cannot help; the caller
 * should stop rather than spin.
 */
int sgug_oauth_registration_gone(const sgug_oauth *o);

#endif /* SGUG_PROTO_OAUTH_H */
