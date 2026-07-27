#include "net/tls.h"

#include "compat/irix.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if defined(__sgi)
#define DEFAULT_CA_BUNDLE "/usr/sgug/etc/pki/tls/cert.pem"
#else
#define DEFAULT_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#endif

/* OpenSSL's read and write take int lengths; size_t is wider on some hosts. */
#define MAX_CHUNK 0x100000

/*
 * R4k through R16k have no AES instructions, so AES-GCM is entirely software
 * while ChaCha20-Poly1305 is not. GitHub offers both. Ordering matters for
 * TLS 1.2; TLS 1.3 suites are set separately below.
 */
#define CIPHERS_TLS12 \
	"ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:" \
	"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:" \
	"ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384"

#define CIPHERS_TLS13 \
	"TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384"

struct sgug_tls_ctx {
	SSL_CTX *ssl_ctx;
};

struct sgug_tls {
	SSL *ssl;
	int fd;
};

/* IRIX rld has no __tls_get_addr, so __thread and _Thread_local fail at first
 * use rather than at link time. pthread keys are the only option. */
static pthread_key_t errkey;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void
free_errbuf(void *p)
{
	free(p);
}

static void
init_globals(void)
{
	pthread_key_create(&errkey, free_errbuf);

	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
}

static void
set_error(const char *what)
{
	unsigned long e;
	char *buf;

	buf = pthread_getspecific(errkey);
	if (buf == NULL) {
		buf = malloc(256);
		if (buf == NULL)
			return;
		pthread_setspecific(errkey, buf);
	}

	e = ERR_get_error();
	if (e != 0) {
		char detail[160];

		ERR_error_string_n(e, detail, sizeof(detail));
		sgug_snprintf(buf, 256, "%s: %s", what, detail);
	} else {
		sgug_snprintf(buf, 256, "%s", what);
	}
	ERR_clear_error();
}

const char *
sgug_tls_last_error(void)
{
	const char *buf = pthread_getspecific(errkey);

	return buf != NULL ? buf : "";
}

sgug_tls_ctx *
sgug_tls_ctx_new(const char *ca_bundle)
{
	sgug_tls_ctx *ctx;

	pthread_once(&init_once, init_globals);

	if (ca_bundle == NULL)
		ca_bundle = getenv("SSL_CERT_FILE");
	if (ca_bundle == NULL)
		ca_bundle = DEFAULT_CA_BUNDLE;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->ssl_ctx = SSL_CTX_new(SSLv23_client_method());
	if (ctx->ssl_ctx == NULL) {
		set_error("SSL_CTX_new");
		free(ctx);
		return NULL;
	}

	if (SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION) != 1) {
		set_error("set_min_proto_version");
		goto fail;
	}

	SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_AUTO_RETRY);
	SSL_CTX_set_session_cache_mode(ctx->ssl_ctx, SSL_SESS_CACHE_CLIENT);

	if (SSL_CTX_set_cipher_list(ctx->ssl_ctx, CIPHERS_TLS12) != 1) {
		set_error("set_cipher_list");
		goto fail;
	}
	if (SSL_CTX_set_ciphersuites(ctx->ssl_ctx, CIPHERS_TLS13) != 1) {
		set_error("set_ciphersuites");
		goto fail;
	}

	if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_bundle, NULL) != 1) {
		set_error("load CA bundle");
		goto fail;
	}

	return ctx;

fail:
	SSL_CTX_free(ctx->ssl_ctx);
	free(ctx);
	return NULL;
}

void
sgug_tls_ctx_free(sgug_tls_ctx *ctx)
{
	if (ctx == NULL)
		return;
	SSL_CTX_free(ctx->ssl_ctx);
	free(ctx);
}

sgug_tls *
sgug_tls_connect(sgug_tls_ctx *ctx, int fd, const char *host)
{
	sgug_tls *tls;

	tls = calloc(1, sizeof(*tls));
	if (tls == NULL)
		return NULL;

	tls->fd = fd;
	tls->ssl = SSL_new(ctx->ssl_ctx);
	if (tls->ssl == NULL) {
		set_error("SSL_new");
		free(tls);
		return NULL;
	}

	/* Both are required and neither is default. Without SNI the edge serves
	 * the wrong certificate for *.github.com; without set1_host OpenSSL
	 * validates the chain but not that it belongs to this host. */
	if (SSL_set_tlsext_host_name(tls->ssl, host) != 1) {
		set_error("SNI");
		goto fail;
	}
	if (SSL_set1_host(tls->ssl, host) != 1) {
		set_error("set1_host");
		goto fail;
	}

	SSL_set_fd(tls->ssl, fd);

	if (SSL_connect(tls->ssl) != 1) {
		long v = SSL_get_verify_result(tls->ssl);

		if (v != X509_V_OK)
			set_error(X509_verify_cert_error_string(v));
		else
			set_error("SSL_connect");
		goto fail;
	}

	return tls;

fail:
	SSL_free(tls->ssl);
	free(tls);
	return NULL;
}

void
sgug_tls_free(sgug_tls *tls)
{
	if (tls == NULL)
		return;

	SSL_shutdown(tls->ssl);
	SSL_free(tls->ssl);
	if (tls->fd >= 0)
		close(tls->fd);
	free(tls);
}

int
sgug_tls_read(sgug_tls *tls, void *buf, size_t len)
{
	int n;

	if (len == 0)
		return 0;
	if (len > MAX_CHUNK)
		len = MAX_CHUNK;

	n = SSL_read(tls->ssl, buf, (int)len);
	if (n > 0)
		return n;

	switch (SSL_get_error(tls->ssl, n)) {
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_SYSCALL:
		/* A truncated close is routine with `Connection: close`. */
		if (ERR_peek_error() == 0)
			return 0;
		/* fallthrough */
	default:
		set_error("SSL_read");
		return -1;
	}
}

int
sgug_tls_write(sgug_tls *tls, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;

	while (sent < len) {
		size_t chunk = len - sent;
		int n;

		if (chunk > MAX_CHUNK)
			chunk = MAX_CHUNK;

		n = SSL_write(tls->ssl, p + sent, (int)chunk);
		if (n <= 0) {
			set_error("SSL_write");
			return -1;
		}
		sent += (size_t)n;
	}
	return (int)sent;
}

const char *
sgug_tls_version(const sgug_tls *tls)
{
	return SSL_get_version(tls->ssl);
}

const char *
sgug_tls_cipher(const sgug_tls *tls)
{
	return SSL_get_cipher(tls->ssl);
}
