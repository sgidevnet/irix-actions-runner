#ifndef SGUG_PROTO_LISTENER_H
#define SGUG_PROTO_LISTENER_H

#include "crypto/rsa.h"
#include "net/http.h"
#include "proto/config.h"
#include "proto/oauth.h"

#include <signal.h>
#include <stddef.h>

/*
 * The listen loop: create a session, long poll for messages, decrypt, dispatch.
 *
 * A session is what makes the runner show Online in the GitHub UI. Without one
 * the agent record exists but is dark.
 */

typedef struct {
	const char *type;	/* messageType, e.g. PipelineAgentJobRequest */
	int64_t message_id;
	const char *body;	/* decrypted plaintext, UTF-8 JSON */
	size_t body_len;
} sgug_message;

/*
 * Returns 0 to continue listening, non-zero to stop. Called after the message
 * has already been deleted server side, so a handler that crashes will not see
 * the same message redelivered forever.
 */
typedef int (*sgug_message_fn)(void *ctx, const sgug_message *msg);

typedef struct {
	sgug_http_client *http;
	const sgug_config *cfg;
	sgug_oauth *oauth;
	const sgug_rsa *key;

	sgug_message_fn on_message;
	void *ctx;

	/* Set from a signal handler to request a clean shutdown. */
	volatile sig_atomic_t *stop;

	/* 0 for quiet, 1 for per-message, 2 for per-poll. */
	int verbose;
} sgug_listener_opts;

/*
 * Runs until *stop is set, the handler returns non-zero, or the registration
 * is found to be gone. Deletes the session on the way out so the runner shows
 * Offline promptly rather than lingering until the service times it out.
 */
int sgug_listen(const sgug_listener_opts *opts, char *err, size_t errlen);

#endif /* SGUG_PROTO_LISTENER_H */
