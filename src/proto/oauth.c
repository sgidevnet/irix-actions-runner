#include "proto/oauth.h"

#include "crypto/jwt.h"
#include "json/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Renew this long before the stated expiry, so a token cannot lapse during a
 * long poll that may itself take 50 seconds. */
#define RENEW_MARGIN 60

/*
 * grant_type is client_credentials. The jwt-bearer URN appears only as the
 * client_assertion_type. Getting these the other way round is a documented
 * trap; the constant for a jwt-bearer grant exists in the reference SDK but
 * the runner path does not use it.
 */
#define FORM_PREFIX \
	"grant_type=client_credentials" \
	"&client_assertion_type=" \
	"urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer" \
	"&client_assertion="

struct sgug_oauth {
	sgug_http_client *http;
	const sgug_config *cfg;
	const sgug_rsa *key;

	char *token;
	sgug_time_t expires_at;
	int registration_gone;
};

static void
seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;

	va_start(ap, fmt);
	sgug_vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

sgug_oauth *
sgug_oauth_new(sgug_http_client *http, const sgug_config *cfg,
    const sgug_rsa *key)
{
	sgug_oauth *o = calloc(1, sizeof(*o));

	if (o == NULL)
		return NULL;
	o->http = http;
	o->cfg = cfg;
	o->key = key;
	return o;
}

void
sgug_oauth_free(sgug_oauth *o)
{
	if (o == NULL)
		return;
	if (o->token != NULL) {
		explicit_bzero(o->token, strlen(o->token));
		free(o->token);
	}
	free(o);
}

void
sgug_oauth_invalidate(sgug_oauth *o)
{
	if (o == NULL || o->token == NULL)
		return;
	explicit_bzero(o->token, strlen(o->token));
	free(o->token);
	o->token = NULL;
	o->expires_at = 0;
}

int
sgug_oauth_registration_gone(const sgug_oauth *o)
{
	return o != NULL && o->registration_gone;
}

static int
mint(sgug_oauth *o, char *err, size_t errlen)
{
	char assertion[1400];
	char *form = NULL;
	const char *headers[2];
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *root;
	const char *body, *tok;
	size_t body_len, form_len;
	sgug_time_t now;
	int n, rc = -1;

	/*
	 * Corrected for measured clock offset. The assertion is valid for five
	 * minutes and the service rejects a skew beyond that outright, so on a
	 * vintage machine with an unset clock this is what makes the request
	 * succeed at all rather than returning an opaque 401.
	 */
	now = sgug_http_now(o->http);

	n = sgug_jwt_client_assertion(o->key, o->cfg->client_id,
	    o->cfg->auth_url, now, o->cfg->require_fips,
	    assertion, sizeof(assertion));
	if (n < 0) {
		seterr(err, errlen, "cannot build client assertion");
		return -1;
	}

	form_len = strlen(FORM_PREFIX) + (size_t)n;
	form = malloc(form_len + 1);
	if (form == NULL)
		return -1;
	/* The assertion is base64url, so it needs no percent encoding. */
	memcpy(form, FORM_PREFIX, strlen(FORM_PREFIX));
	memcpy(form + strlen(FORM_PREFIX), assertion, (size_t)n);
	form[form_len] = '\0';

	headers[0] = "Content-Type: application/x-www-form-urlencoded; charset=utf-8";
	headers[1] = "Accept: application/json";

	if (sgug_http_request(o->http, "POST", o->cfg->auth_url, headers, 2,
	    form, form_len, 30000, &r) != 0) {
		seterr(err, errlen, "token request: %s", sgug_http_last_error());
		goto out;
	}

	body = sgug_http_body(r, &body_len);

	if (sgug_http_status(r) != 200) {
		/*
		 * invalid_client means the runner was deleted server side.
		 * Retrying will never succeed, so surface it distinctly rather
		 * than letting the caller spin on backoff forever.
		 */
		if (strstr(body, "invalid_client") != NULL) {
			o->registration_gone = 1;
			seterr(err, errlen,
			    "the service no longer knows this runner; it was "
			    "probably removed in the GitHub UI. Re-run configure");
		} else {
			seterr(err, errlen, "token request returned %d: %.200s",
			    sgug_http_status(r), body);
		}
		goto out;
	}

	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL) {
		seterr(err, errlen, "token response was not JSON");
		goto out;
	}

	root = sgug_json_root(doc);
	tok = sgug_json_string(sgug_json_get(root, "access_token"), NULL);
	if (tok == NULL) {
		seterr(err, errlen, "token response had no access_token");
		goto out;
	}

	sgug_oauth_invalidate(o);
	o->token = strdup(tok);
	if (o->token == NULL)
		goto out;

	o->expires_at = now +
	    sgug_json_int(sgug_json_get(root, "expires_in"), 600);
	rc = 0;

out:
	if (form != NULL) {
		explicit_bzero(form, form_len);
		free(form);
	}
	explicit_bzero(assertion, sizeof(assertion));
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	return rc;
}

const char *
sgug_oauth_token(sgug_oauth *o, char *err, size_t errlen)
{
	if (o->registration_gone) {
		seterr(err, errlen, "runner registration no longer exists");
		return NULL;
	}

	if (o->token != NULL &&
	    sgug_http_now(o->http) < o->expires_at - RENEW_MARGIN)
		return o->token;

	if (mint(o, err, errlen) != 0)
		return NULL;
	return o->token;
}
