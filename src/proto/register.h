#ifndef SGUG_PROTO_REGISTER_H
#define SGUG_PROTO_REGISTER_H

#include "net/http.h"
#include "proto/config.h"

#include <stddef.h>

typedef struct {
	const char *github_url;		/* https://github.com/org or .../org/repo */
	const char *reg_token;		/* short lived, from the UI or the API */
	const char *runner_name;
	const char *runner_group;	/* pool name; NULL means Default */
	const char *work_folder;
	const char **labels;		/* extra user labels beyond the defaults */
	size_t nlabels;
	int replace;			/* replace an existing runner of the same name */
} sgug_register_opts;

/*
 * Runs the full registration and writes the configuration into dir.
 *
 * Three calls, in order:
 *   1. POST {api}/actions/runner-registration, authorised with the
 *      registration token, exchanging it for the tenant URL and a bearer token
 *      for the Actions service. This is the only use of the registration
 *      token, and it is not persisted.
 *   2. GET  {tenant}_apis/distributedtask/pools, to resolve the runner group
 *      name to a pool id.
 *   3. POST {tenant}_apis/distributedtask/pools/{poolId}/agents, carrying a
 *      freshly generated RSA public key, which returns the durable OAuth
 *      client id and token endpoint.
 *
 * After step 3 the registration token is irrelevant; the key and client id are
 * what authenticate from then on.
 *
 * Returns 0 on success. On failure returns -1 and writes a description to err.
 */
int sgug_register(sgug_http_client *http, const sgug_register_opts *opts,
    const char *dir, sgug_config *out, char *err, size_t errlen);

/*
 * Deletes the runner from the service and removes the local configuration.
 * Needs a fresh removal token, since the OAuth credentials cannot authorise
 * their own deletion.
 */
int sgug_unregister(sgug_http_client *http, const char *dir,
    const char *removal_token, char *err, size_t errlen);

#endif /* SGUG_PROTO_REGISTER_H */
