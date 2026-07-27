#include "crypto/jwt.h"

#include "crypto/b64.h"

#include <stdio.h>
#include <string.h>

#include <openssl/rand.h>

#define ASSERTION_LIFETIME 300
#define NBF_BACKDATE 30
#define SIG_MAX 512

/* A GUID-shaped jti. Only uniqueness matters; the service treats it as opaque
 * and uses it for replay rejection. */
static int
make_jti(char *out, size_t outlen)
{
	unsigned char b[16];
	int i, o = 0;
	static const char HEX[] = "0123456789abcdef";
	static const int DASH_AFTER[] = { 4, 6, 8, 10 };

	if (outlen < 37 || RAND_bytes(b, sizeof(b)) != 1)
		return -1;

	for (i = 0; i < 16; i++) {
		int d;

		out[o++] = HEX[b[i] >> 4];
		out[o++] = HEX[b[i] & 0x0f];

		for (d = 0; d < 4; d++) {
			if (i + 1 == DASH_AFTER[d])
				out[o++] = '-';
		}
	}
	out[o] = '\0';
	return o;
}

int
sgug_jwt_client_assertion(const sgug_rsa *key, const char *client_id,
    const char *audience, sgug_time_t now, int use_pss,
    char *out, size_t outlen)
{
	char header[64];
	char payload[768];
	char jti[40];
	char nbfbuf[24], expbuf[24];
	unsigned char sig[SIG_MAX];
	sgug_time_t nbf, exp;
	int n, siglen, used = 0;

	if (make_jti(jti, sizeof(jti)) < 0)
		return -1;

	nbf = now - NBF_BACKDATE;
	exp = now + ASSERTION_LIFETIME;

	n = sgug_snprintf(header, sizeof(header),
	    "{\"typ\":\"JWT\",\"alg\":\"%s\"}", use_pss ? "PS256" : "RS256");

	n = sgug_b64url_encode(header, (size_t)n, out, outlen);
	if (n < 0)
		return -1;
	used = n;

	if ((size_t)used + 1 >= outlen)
		return -1;
	out[used++] = '.';

	/* Formatted separately rather than with a printf length modifier: long
	 * is 32 bits under n32, so these would wrap in 2038. */
	sgug_i64toa(nbf, nbfbuf, sizeof(nbfbuf));
	sgug_i64toa(exp, expbuf, sizeof(expbuf));

	n = sgug_snprintf(payload, sizeof(payload),
	    "{\"iss\":\"%s\",\"sub\":\"%s\",\"aud\":\"%s\","
	    "\"jti\":\"%s\",\"nbf\":%s,\"exp\":%s}",
	    client_id, client_id, audience, jti, nbfbuf, expbuf);

	n = sgug_b64url_encode(payload, (size_t)n, out + used, outlen - (size_t)used);
	if (n < 0)
		return -1;
	used += n;

	/* The signing input is the two encoded segments joined by a dot, which
	 * is precisely what has been written so far. */
	siglen = sgug_rsa_sign_sha256(key, out, (size_t)used, use_pss,
	    sig, sizeof(sig));
	if (siglen < 0)
		return -1;

	if ((size_t)used + 1 >= outlen)
		return -1;
	out[used++] = '.';

	n = sgug_b64url_encode(sig, (size_t)siglen, out + used,
	    outlen - (size_t)used);
	if (n < 0)
		return -1;
	used += n;

	return used;
}
