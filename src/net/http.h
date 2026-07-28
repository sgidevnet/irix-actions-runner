#ifndef SGUG_NET_HTTP_H
#define SGUG_NET_HTTP_H

#include "compat/irix.h"
#include "net/tls.h"

#include <stddef.h>

/*
 * HTTP/1.1 client, sized for this protocol rather than for generality.
 *
 * The client keeps one idle connection and reuses it when the next request
 * goes to the same host. That is not a micro-optimisation here: an ECDHE
 * P-256 handshake costs tens of milliseconds on an R12000, because n32 has no
 * __int128 and therefore no fast NIST P-256 path in OpenSSL, and the listener
 * reconnects every 50 seconds forever.
 */

typedef struct sgug_http_client sgug_http_client;
typedef struct sgug_http_resp sgug_http_resp;

/* user_agent must be non-empty. GitHub rejects requests without one. */
sgug_http_client *sgug_http_client_new(const char *ca_bundle,
    const char *user_agent);
void sgug_http_client_free(sgug_http_client *c);

/*
 * The client's TLS context, so another connection can share the trust store
 * rather than parse it again. Loading a CA bundle is not free on these
 * machines. Borrowed: it dies with the client.
 */
sgug_tls_ctx *sgug_http_tls_ctx(sgug_http_client *c);

/*
 * headers is an array of "Name: value" strings, nheaders long. body may be
 * NULL. timeout_ms bounds each socket read; the long poll needs at least
 * 100000, since the service holds the connection open for about 50 seconds
 * before answering empty.
 *
 * Returns 0 and sets *out on any complete HTTP response, including 4xx and
 * 5xx; the status is the caller's to interpret. Returns -1 only on transport
 * failure, with a description in sgug_http_last_error.
 *
 * Cross-host redirects are followed, up to 10.
 */
int sgug_http_request(sgug_http_client *c, const char *method, const char *url,
    const char *const *headers, size_t nheaders,
    const void *body, size_t body_len, int timeout_ms,
    sgug_http_resp **out);

int sgug_http_status(const sgug_http_resp *r);

/* Body bytes, always NUL terminated. *len may be NULL. Empty for 204. */
const char *sgug_http_body(const sgug_http_resp *r, size_t *len);

/* Case-insensitive header lookup, since HTTP header names are not case
 * sensitive and the service is inconsistent about casing. */
const char *sgug_http_header(const sgug_http_resp *r, const char *name);

void sgug_http_resp_free(sgug_http_resp *r);

/*
 * Offset between this machine's clock and the server's, in seconds, positive
 * when we are ahead. Updated from the Date header of every response.
 *
 * Only meaningful past roughly a minute. Two independently NTP-disciplined
 * machines both measure themselves about 7 seconds ahead of api.github.com,
 * so the header is a coarse reference and small values are not real error.
 */
sgug_time_t sgug_http_skew(const sgug_http_client *c);

/* Corrected current time: the system clock less the measured skew, applied
 * only once the skew exceeds the threshold above. Use this for JWT claims. */
sgug_time_t sgug_http_now(const sgug_http_client *c);

/*
 * Optional shutdown check, consulted whenever a socket read times out with the
 * response still incomplete. Return non-zero to abandon the request.
 *
 * Without it a request blocked on the 50 second long poll cannot be
 * interrupted, so a SIGTERM takes up to a full poll cycle to take effect and
 * service management looks hung.
 */
void sgug_http_set_abort_check(sgug_http_client *c, int (*cb)(void *),
    void *ctx);

const char *sgug_http_last_error(void);

#endif /* SGUG_NET_HTTP_H */
