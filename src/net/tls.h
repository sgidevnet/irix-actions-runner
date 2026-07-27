#ifndef SGUG_NET_TLS_H
#define SGUG_NET_TLS_H

#include <stddef.h>

typedef struct sgug_tls_ctx sgug_tls_ctx;
typedef struct sgug_tls sgug_tls;

/*
 * Creates a client context pinned to TLS 1.2 or better with peer verification
 * on. ca_bundle is a PEM file; pass NULL to use the compiled-in default.
 *
 * Contexts are reusable and hold the session cache. Share one across
 * connections: an ECDHE P-256 handshake costs tens of milliseconds on an
 * R12000, since n32 has no __int128 and so no fast NIST P-256 path, and
 * resumption avoids paying it on every reconnect.
 */
sgug_tls_ctx *sgug_tls_ctx_new(const char *ca_bundle);
void sgug_tls_ctx_free(sgug_tls_ctx *ctx);

/*
 * Wraps a connected socket. host is used for both SNI and hostname
 * verification; *.github.com requires SNI, and OpenSSL does not verify the
 * hostname unless asked. Takes ownership of fd on success.
 *
 * Returns NULL on handshake or verification failure; call sgug_tls_last_error
 * for a description.
 */
sgug_tls *sgug_tls_connect(sgug_tls_ctx *ctx, int fd, const char *host);
void sgug_tls_free(sgug_tls *tls);

/* Return bytes transferred, 0 at clean EOF, or -1. Both retry the OpenSSL
 * WANT_READ/WANT_WRITE cycle internally, so a short return is real. */
int sgug_tls_read(sgug_tls *tls, void *buf, size_t len);
int sgug_tls_write(sgug_tls *tls, const void *buf, size_t len);

/* Negotiated protocol and cipher, for diagnostics. Valid while tls lives. */
const char *sgug_tls_version(const sgug_tls *tls);
const char *sgug_tls_cipher(const sgug_tls *tls);

/* Last error on this thread, or "" if none. */
const char *sgug_tls_last_error(void);

#endif /* SGUG_NET_TLS_H */
