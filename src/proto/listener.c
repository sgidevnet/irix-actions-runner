#include "proto/listener.h"

#include "crypto/aes.h"
#include "crypto/b64.h"
#include "json/json.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define API_SESSIONS "5.1-preview.1"
#define API_MESSAGES "6.0-preview.1"
#define API_DELETE "5.1-preview.1"

/*
 * The service holds the long poll open for about 50 seconds before answering
 * empty, so the socket timeout has to comfortably exceed that. A 30 second
 * timeout here looks like a flapping network and never receives a job.
 */
#define POLL_TIMEOUT_MS 120000
#define SHORT_TIMEOUT_MS 30000

/* Session key is AES-256. */
#define SESSION_KEY_LEN 32

struct session {
	char id[64];
	unsigned char key[SESSION_KEY_LEN];
	int have_key;
};

/*
 * State that outlives an individual session.
 *
 * broker_url is set when the service sends a BrokerMigration, which it does
 * even for an org whose registration reported use_v2_flow false. Once set,
 * sessions and polls go to the broker instead of the pool. A runner that
 * ignores the migration keeps polling the pool, receives nothing but repeated
 * migration notices, and never sees a job.
 */
struct lstate {
	struct session s;
	char broker_url[SGUG_MAX_URL];
};

/* Joins the broker base and a path, tolerating a trailing slash on the base. */
static void
broker_url(const struct lstate *st, const char *leaf, char *out, size_t outlen)
{
	size_t n = strlen(st->broker_url);

	if (n > 0 && st->broker_url[n - 1] == '/')
		sgug_snprintf(out, outlen, "%s%s", st->broker_url, leaf);
	else
		sgug_snprintf(out, outlen, "%s/%s", st->broker_url, leaf);
}

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

/*
 * Sleeps in one second steps so a shutdown signal is noticed promptly rather
 * than after the whole backoff has elapsed.
 */
static void
backoff_sleep(int seconds, volatile sig_atomic_t *stop)
{
	int i;

	for (i = 0; i < seconds; i++) {
		if (stop != NULL && *stop)
			return;
		sleep(1);
	}
}

static int
auth_header(sgug_oauth *oauth, char *out, size_t outlen, char *err,
    size_t errlen)
{
	const char *tok = sgug_oauth_token(oauth, err, errlen);

	if (tok == NULL)
		return -1;
	sgug_snprintf(out, outlen, "Authorization: Bearer %s", tok);
	return 0;
}

/*
 * Creates the agent session and recovers the AES key the service encrypted to
 * our public key.
 */
static int
session_create(const sgug_listener_opts *o, struct lstate *st, char *err,
    size_t errlen)
{
	struct session *s = &st->s;
	char url[SGUG_MAX_URL];
	char auth[2048];
	char owner[192];
	char hostname[128];
	const char *headers[3];
	sgug_jsonw *w;
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *root, *enc;
	const char *body, *val;
	unsigned char raw[512];
	size_t body_len;
	int n, rc = -1;

	memset(s, 0, sizeof(*s));

	if (gethostname(hostname, sizeof(hostname)) != 0)
		sgug_snprintf(hostname, sizeof(hostname), "irix");
	hostname[sizeof(hostname) - 1] = '\0';
	sgug_snprintf(owner, sizeof(owner), "%s (PID: %ld)", hostname,
	    (long)getpid());

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "ownerName");
	sgug_jsonw_str(w, owner);
	sgug_jsonw_key(w, "agent");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "id");
	sgug_jsonw_int(w, o->cfg->agent_id);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, o->cfg->agent_name);
	sgug_jsonw_key(w, "version");
	sgug_jsonw_str(w, SGUG_RUNNER_VERSION);
	sgug_jsonw_key(w, "osDescription");
	sgug_jsonw_str(w, "IRIX 6.5 mips");
	sgug_jsonw_obj_end(w);
	/*
	 * Always false. The reference Go runner hard-sets it with a note that
	 * github.com reset it to false in 2021 and that GHES 3.0.11 requires
	 * false; sending true yields a key we cannot unwrap.
	 */
	sgug_jsonw_key(w, "useFipsEncryption");
	sgug_jsonw_bool(w, 0);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	if (auth_header(o->oauth, auth, sizeof(auth), err, errlen) != 0)
		goto out;

	headers[0] = auth;
	headers[1] = "Accept: application/json; api-version=" API_SESSIONS;
	headers[2] = "Content-Type: application/json; charset=utf-8; api-version="
	    API_SESSIONS;

	if (st->broker_url[0] != '\0') {
		/* The broker takes no api-version at all; sending one is
		 * harmless but the plain Accept is what it expects. */
		broker_url(st, "session", url, sizeof(url));
		headers[1] = "Accept: application/json";
		headers[2] = "Content-Type: application/json; charset=utf-8";
	} else {
		sgug_snprintf(url, sizeof(url),
		    "%s_apis/distributedtask/pools/%ld/sessions",
		    o->cfg->server_url, (long)o->cfg->pool_id);
	}

	if (sgug_http_request(o->http, "POST", url, headers, 3, body, body_len,
	    SHORT_TIMEOUT_MS, &r) != 0) {
		seterr(err, errlen, "create session: %s", sgug_http_last_error());
		goto out;
	}

	if (sgug_http_status(r) == 401 || sgug_http_status(r) == 400) {
		sgug_oauth_invalidate(o->oauth);
		seterr(err, errlen, "create session returned %d, will retry",
		    sgug_http_status(r));
		goto out;
	}
	if (sgug_http_status(r) == 409) {
		/* Another session for this agent is still alive; the service
		 * expires it within a few minutes. */
		seterr(err, errlen, "another session already exists for this "
		    "runner, waiting for it to expire");
		goto out;
	}
	/* The pool answers 200, the broker 201. */
	if (sgug_http_status(r) != 200 && sgug_http_status(r) != 201) {
		seterr(err, errlen, "create session returned %d: %.200s",
		    sgug_http_status(r), sgug_http_body(r, NULL));
		goto out;
	}

	body = sgug_http_body(r, &body_len);
	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL) {
		seterr(err, errlen, "session response was not JSON");
		goto out;
	}
	root = sgug_json_root(doc);

	/*
	 * The session response itself can redirect us to the broker, in which
	 * case the session just created is not the one to poll. Record the base
	 * and report failure so the caller recreates against it.
	 */
	{
		const char *bu = sgug_json_string(
		    sgug_json_path(root, "brokerMigrationMessage.brokerBaseUrl"),
		    NULL);

		if (bu != NULL && *bu != '\0' &&
		    strcmp(bu, st->broker_url) != 0) {
			sgug_snprintf(st->broker_url, sizeof(st->broker_url),
			    "%s", bu);
			seterr(err, errlen,
			    "service redirected the session to the broker");
			goto out;
		}
	}

	sgug_snprintf(s->id, sizeof(s->id), "%s",
	    sgug_json_string(sgug_json_get(root, "sessionId"), ""));
	if (s->id[0] == '\0') {
		seterr(err, errlen, "session response had no sessionId");
		goto out;
	}

	/*
	 * The broker sends no encryptionKey and its messages arrive in clear;
	 * only the pool wraps an AES key to our public key.
	 */
	enc = sgug_json_get(root, "encryptionKey");
	val = sgug_json_string(sgug_json_get(enc, "value"), NULL);
	if (val != NULL && *val != '\0') {
		n = sgug_b64_decode(val, raw, sizeof(raw));
		if (n < 0) {
			seterr(err, errlen, "session key was not valid base64");
			goto out;
		}

		if (sgug_json_bool(sgug_json_get(enc, "encrypted"), 1)) {
			/*
			 * RSA-OAEP with SHA-1, not PKCS#1 v1.5, because we sent
			 * useFipsEncryption false. SHA-256 would be the FIPS
			 * variant.
			 */
			int ctlen = n;

			/*
			 * SHA-256 first. The reference documentation says the
			 * OAEP digest follows useFipsEncryption, so SHA-1 when
			 * false, but github.com wraps with SHA-256 regardless;
			 * measured against a live session, SHA-1 fails and
			 * SHA-256 yields the expected 32 bytes. SHA-1 remains as
			 * a fallback for older GHES.
			 */
			n = sgug_rsa_decrypt_oaep(o->key, raw, (size_t)ctlen, 1,
			    s->key, sizeof(s->key));
			if (n != SESSION_KEY_LEN)
				n = sgug_rsa_decrypt_oaep(o->key, raw,
				    (size_t)ctlen, 0, s->key, sizeof(s->key));

			if (n != SESSION_KEY_LEN) {
				seterr(err, errlen,
				    "could not unwrap the session key: %d byte "
				    "ciphertext yielded %d, wanted %d; the "
				    "service may hold a different public key",
				    ctlen, n, SESSION_KEY_LEN);
				goto out;
			}
		} else {
			if (n != SESSION_KEY_LEN) {
				seterr(err, errlen, "session key was %d bytes, "
				    "expected %d", n, SESSION_KEY_LEN);
				goto out;
			}
			memcpy(s->key, raw, SESSION_KEY_LEN);
		}
		s->have_key = 1;
	}

	rc = 0;

out:
	explicit_bzero(raw, sizeof(raw));
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	sgug_jsonw_free(w);
	return rc;
}

static void
session_delete(const sgug_listener_opts *o, const struct lstate *st)
{
	const struct session *s = &st->s;
	char url[SGUG_MAX_URL];
	char auth[2048];
	const char *headers[2];
	sgug_http_resp *r = NULL;

	if (s->id[0] == '\0')
		return;
	if (auth_header(o->oauth, auth, sizeof(auth), NULL, 0) != 0)
		return;

	headers[0] = auth;
	headers[1] = "Accept: application/json; api-version=" API_SESSIONS;

	if (st->broker_url[0] != '\0') {
		char base[SGUG_MAX_URL];

		/* The broker route carries no session id in the path, so it
		 * goes in the query string. */
		broker_url(st, "session", base, sizeof(base));
		sgug_snprintf(url, sizeof(url), "%s?sessionId=%s", base, s->id);
		headers[1] = "Accept: application/json";
	} else {
		sgug_snprintf(url, sizeof(url),
		    "%s_apis/distributedtask/pools/%ld/sessions/%s",
		    o->cfg->server_url, (long)o->cfg->pool_id, s->id);
	}

	if (sgug_http_request(o->http, "DELETE", url, headers, 2, NULL, 0,
	    SHORT_TIMEOUT_MS, &r) == 0)
		sgug_http_resp_free(r);
}

/*
 * Deleting is what stops redelivery, and the reference implementations do it
 * as soon as the message is in hand rather than after processing. A handler
 * that dies mid-job would otherwise receive the same message on every restart.
 */
static void
message_delete(const sgug_listener_opts *o, const struct lstate *st,
    int64_t message_id)
{
	const struct session *s = &st->s;
	char url[SGUG_MAX_URL];
	char auth[2048];
	char idbuf[24];
	const char *headers[2];
	sgug_http_resp *r = NULL;

	/* The broker has no delete; acknowledgement there is a separate POST
	 * carrying the runner request id, and only for job requests. */
	if (st->broker_url[0] != '\0')
		return;

	if (auth_header(o->oauth, auth, sizeof(auth), NULL, 0) != 0)
		return;

	headers[0] = auth;
	headers[1] = "Accept: application/json; api-version=" API_DELETE;

	sgug_i64toa(message_id, idbuf, sizeof(idbuf));
	sgug_snprintf(url, sizeof(url),
	    "%s_apis/distributedtask/pools/%ld/messages/%s?sessionId=%s",
	    o->cfg->server_url, (long)o->cfg->pool_id, idbuf, s->id);

	if (sgug_http_request(o->http, "DELETE", url, headers, 2, NULL, 0,
	    SHORT_TIMEOUT_MS, &r) == 0)
		sgug_http_resp_free(r);
}

/*
 * One long poll. Returns 1 when a message was dispatched, 0 when the poll
 * returned empty, and -1 on an error the caller should back off from.
 */
static int
poll_once(const sgug_listener_opts *o, struct lstate *st, int64_t *last_id,
    int *handler_stop, char *err, size_t errlen)
{
	struct session *s = &st->s;
	char url[SGUG_MAX_URL + 256];
	char auth[2048];
	char lastbuf[24];
	const char *headers[2];
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	char *plain = NULL;
	const sgug_json *root;
	const char *body, *type, *iv_b64, *body_b64;
	size_t body_len;
	int status, rc = -1;

	if (auth_header(o->oauth, auth, sizeof(auth), err, errlen) != 0)
		return -1;

	headers[0] = auth;
	headers[1] = "Accept: application/json; api-version=" API_MESSAGES;

	if (st->broker_url[0] != '\0') {
		char base[SGUG_MAX_URL];

		broker_url(st, "message", base, sizeof(base));
		headers[1] = "Accept: application/json";
		sgug_snprintf(url, sizeof(url),
		    "%s?sessionId=%s&status=Online&runnerVersion=%s"
		    "&os=Linux&architecture=X64&disableUpdate=true",
		    base, s->id, SGUG_RUNNER_VERSION);
	} else {
		sgug_snprintf(url, sizeof(url),
		    "%s_apis/distributedtask/pools/%ld/messages?sessionId=%s"
		    "&status=Online&runnerVersion=%s&os=Linux&architecture=X64"
		    "&disableUpdate=true", o->cfg->server_url,
		    (long)o->cfg->pool_id, s->id, SGUG_RUNNER_VERSION);
	}

	if (*last_id != 0) {
		size_t used = strlen(url);

		sgug_i64toa(*last_id, lastbuf, sizeof(lastbuf));
		sgug_snprintf(url + used, sizeof(url) - used, "&lastMessageId=%s",
		    lastbuf);
	}

	if (sgug_http_request(o->http, "GET", url, headers, 2, NULL, 0,
	    POLL_TIMEOUT_MS, &r) != 0) {
		seterr(err, errlen, "poll: %s", sgug_http_last_error());
		return -1;
	}

	status = sgug_http_status(r);

	/* 204 and 202 both mean no message. Poll again immediately; this is the
	 * normal case roughly every 50 seconds. */
	if (status == 204 || status == 202) {
		rc = 0;
		goto out;
	}

	if (status == 401 || status == 400) {
		sgug_oauth_invalidate(o->oauth);
		seterr(err, errlen, "poll returned %d, reauthorising", status);
		goto out;
	}

	if (status == 404) {
		/* The session expired or the agent was deleted. Signal the
		 * caller to rebuild the session. */
		s->id[0] = '\0';
		seterr(err, errlen, "session no longer valid, recreating");
		goto out;
	}

	if (status != 200) {
		seterr(err, errlen, "poll returned %d: %.200s", status,
		    sgug_http_body(r, NULL));
		goto out;
	}

	body = sgug_http_body(r, &body_len);
	if (body_len == 0) {
		rc = 0;
		goto out;
	}

	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL) {
		seterr(err, errlen, "message was not JSON");
		goto out;
	}
	root = sgug_json_root(doc);

	type = sgug_json_string(sgug_json_get(root, "messageType"), "");
	*last_id = sgug_json_int(sgug_json_get(root, "messageId"), *last_id);
	iv_b64 = sgug_json_string(sgug_json_get(root, "iv"), NULL);
	body_b64 = sgug_json_string(sgug_json_get(root, "body"), "");

	message_delete(o, st, *last_id);

	if (iv_b64 != NULL && *iv_b64 != '\0' && s->have_key) {
		unsigned char iv[16];
		unsigned char *ct;
		size_t ct_cap = strlen(body_b64) + 1;
		int ivlen, ctlen, plen;

		ivlen = sgug_b64_decode(iv_b64, iv, sizeof(iv));
		if (ivlen != (int)sizeof(iv)) {
			seterr(err, errlen, "message iv was %d bytes", ivlen);
			goto out;
		}

		ct = malloc(ct_cap);
		plain = malloc(ct_cap + 1);
		if (ct == NULL || plain == NULL) {
			free(ct);
			goto out;
		}

		ctlen = sgug_b64_decode(body_b64, ct, ct_cap);
		if (ctlen < 0) {
			free(ct);
			seterr(err, errlen, "message body was not valid base64");
			goto out;
		}

		plen = sgug_aes_cbc_decrypt(s->key, iv, ct, (size_t)ctlen,
		    plain, ct_cap + 1);
		free(ct);

		if (plen < 0) {
			seterr(err, errlen, "could not decrypt message body");
			goto out;
		}
		body_len = (size_t)plen;
	} else {
		/* An unencrypted body arrives verbatim. RunnerJobRequest in the
		 * v2 flow is delivered this way. */
		plain = strdup(body_b64);
		if (plain == NULL)
			goto out;
		body_len = strlen(plain);
	}

	/*
	 * Handled here rather than by the caller's handler: it changes where we
	 * poll, and the current session belongs to the old endpoint. The service
	 * sends this even when registration reported use_v2_flow false, and a
	 * runner that treats it as an ordinary message polls the pool forever
	 * and never receives a job.
	 */
	if (strcmp(type, "BrokerMigration") == 0) {
		sgug_json_doc *bd = sgug_json_parse(plain, body_len, NULL, 0);
		const char *bu = bd != NULL ? sgug_json_string(
		    sgug_json_get(sgug_json_root(bd), "brokerBaseUrl"), NULL) : NULL;

		if (bu != NULL && *bu != '\0') {
			sgug_snprintf(st->broker_url, sizeof(st->broker_url),
			    "%s", bu);
			printf("migrating to broker %s\n", st->broker_url);
			fflush(stdout);
			/* Force the caller to build a session at the broker. */
			s->id[0] = '\0';
			*last_id = 0;
		} else {
			seterr(err, errlen,
			    "BrokerMigration carried no brokerBaseUrl");
		}
		sgug_json_free(bd);
		rc = 1;
		goto out;
	}

	if (o->verbose >= 1) {
		printf("message   %s (id %ld, %lu bytes)\n", type,
		    (long)*last_id, (unsigned long)body_len);
		fflush(stdout);
	}

	if (o->on_message != NULL) {
		sgug_message msg;

		msg.type = type;
		msg.message_id = *last_id;
		msg.body = plain;
		msg.body_len = body_len;

		if (o->on_message(o->ctx, &msg) != 0)
			*handler_stop = 1;
	}

	rc = 1;

out:
	free(plain);
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	return rc;
}

int
sgug_listen(const sgug_listener_opts *o, char *err, size_t errlen)
{
	struct lstate st;
	int64_t last_id = 0;
	int consecutive_errors = 0;
	int handler_stop = 0;
	int rc = 0;

	memset(&st, 0, sizeof(st));

	while ((o->stop == NULL || !*o->stop) && !handler_stop) {
		char local_err[512];

		if (st.s.id[0] == '\0') {
			local_err[0] = '\0';
			if (session_create(o, &st, local_err,
			    sizeof(local_err)) != 0) {
				if (sgug_oauth_registration_gone(o->oauth)) {
					seterr(err, errlen, "%s", local_err);
					rc = -1;
					break;
				}
				fprintf(stderr, "session: %s\n", local_err);
				consecutive_errors++;
				backoff_sleep(consecutive_errors > 4 ? 60 : 15,
				    o->stop);
				continue;
			}

			consecutive_errors = 0;
			last_id = 0;
			printf("listening as %s in group %s%s\n",
			    o->cfg->agent_name, o->cfg->pool_name,
			    st.broker_url[0] != '\0' ? " via broker" : "");
			fflush(stdout);
		}

		local_err[0] = '\0';
		switch (poll_once(o, &st, &last_id, &handler_stop, local_err,
		    sizeof(local_err))) {
		case 1:
		case 0:
			consecutive_errors = 0;
			if (o->verbose >= 2) {
				printf("poll      ok\n");
				fflush(stdout);
			}
			break;
		default:
			if (sgug_oauth_registration_gone(o->oauth)) {
				seterr(err, errlen, "%s", local_err);
				rc = -1;
				handler_stop = 1;
				break;
			}
			/* A poll abandoned because we are shutting down is the
			 * expected path out, not a failure to report. */
			if (o->stop != NULL && *o->stop)
				break;
			if (local_err[0] != '\0')
				fprintf(stderr, "poll: %s\n", local_err);
			consecutive_errors++;
			/* The reference runner randomises 15 to 30 seconds for
			 * the first few failures then 30 to 60. Fixed steps are
			 * adequate for a single runner. */
			backoff_sleep(consecutive_errors > 4 ? 45 : 15, o->stop);
			break;
		}
	}

	session_delete(o, &st);
	explicit_bzero(&st, sizeof(st));
	return rc;
}
