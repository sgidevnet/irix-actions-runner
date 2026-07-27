#ifndef SGUG_CRYPTO_JWT_H
#define SGUG_CRYPTO_JWT_H

#include "compat/irix.h"
#include "crypto/rsa.h"

#include <stddef.h>

/*
 * Builds the OAuth client assertion.
 *
 * client_id becomes both iss and sub; audience is the token endpoint URL
 * verbatim. use_pss selects PS256 over RS256 and must match the agent's
 * RequireFipsCryptography property.
 *
 * now is our corrected notion of the current time, that is the system clock
 * plus the offset measured from server Date headers. The assertion is valid for
 * five minutes and nbf is backdated 30 seconds, matching the reference runner;
 * the service rejects a skew over five minutes outright. On hardware whose RTC
 * has been dead for twenty years the correction is what makes this work at all.
 *
 * The header carries only alg and typ. There is deliberately no kid and no x5t:
 * the service identifies the key by client_id, and the reference implementation
 * omits both. iat is omitted too, which the reference runner does on purpose.
 *
 * Returns the token length excluding the terminator, or -1. A 2048-bit key
 * produces roughly 600 bytes, so 1024 is a safe buffer.
 */
int sgug_jwt_client_assertion(const sgug_rsa *key, const char *client_id,
    const char *audience, sgug_time_t now, int use_pss,
    char *out, size_t outlen);

#endif /* SGUG_CRYPTO_JWT_H */
