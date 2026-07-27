#include "proto/register.h"

#include "crypto/rsa.h"
#include "json/json.h"

#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_VERSION_POOLS "5.1-preview.1"
#define API_VERSION_AGENTS "6.0-preview.2"

/*
 * The api-version belongs in the Accept header as a media type parameter, not
 * only in the query string. This is the single most common way to get a 400
 * back from this service while sending an otherwise correct request; the
 * generated C# client puts it there and the service honours it.
 */
#define ACCEPT_POOLS "Accept: application/json; api-version=" API_VERSION_POOLS
#define ACCEPT_AGENTS "Accept: application/json; api-version=" API_VERSION_AGENTS
#define CT_AGENTS \
	"Content-Type: application/json; charset=utf-8; api-version=" API_VERSION_AGENTS

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
 * Resolves the GitHub API host for the registration endpoint. github.com and
 * *.ghe.com use the api. subdomain; GHES puts the same paths under /api/v3.
 */
static int
api_base(const char *github_url, char *out, size_t outlen)
{
	char host[256];
	const char *p, *slash;
	size_t n;

	p = strstr(github_url, "://");
	if (p == NULL)
		return -1;
	p += 3;

	slash = strchr(p, '/');
	n = slash != NULL ? (size_t)(slash - p) : strlen(p);
	if (n == 0 || n >= sizeof(host))
		return -1;
	memcpy(host, p, n);
	host[n] = '\0';

	if (strcmp(host, "github.com") == 0 || strcmp(host, "www.github.com") == 0)
		sgug_snprintf(out, outlen, "https://api.github.com");
	else if (strlen(host) > 8 &&
	    strcmp(host + strlen(host) - 8, ".ghe.com") == 0)
		sgug_snprintf(out, outlen, "https://api.%s", host);
	else
		sgug_snprintf(out, outlen, "https://%s/api/v3", host);
	return 0;
}

/* Step 1: exchange the registration token for tenant credentials. */
static int
runner_registration(sgug_http_client *http, const sgug_register_opts *opts,
    char *tenant_url, size_t tenant_len, char **bearer,
    int *use_v2, char *err, size_t errlen)
{
	char base[SGUG_MAX_URL];
	char url[SGUG_MAX_URL];
	char auth[512];
	const char *headers[3];
	sgug_jsonw *w;
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *root;
	const char *body, *tok;
	size_t body_len;
	int rc = -1;

	if (api_base(opts->github_url, base, sizeof(base)) != 0) {
		seterr(err, errlen, "cannot derive API host from %s",
		    opts->github_url);
		return -1;
	}
	sgug_snprintf(url, sizeof(url), "%s/actions/runner-registration", base);

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "url");
	sgug_jsonw_str(w, opts->github_url);
	sgug_jsonw_key(w, "runner_event");
	sgug_jsonw_str(w, "register");
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	/* RemoteAuth, not Bearer or token. The service rejects the others. */
	sgug_snprintf(auth, sizeof(auth), "Authorization: RemoteAuth %s",
	    opts->reg_token);
	headers[0] = auth;
	headers[1] = "Content-Type: application/json";
	headers[2] = "Accept: application/json";

	if (sgug_http_request(http, "POST", url, headers, 3, body, body_len,
	    30000, &r) != 0) {
		seterr(err, errlen, "runner-registration: %s",
		    sgug_http_last_error());
		goto out;
	}

	if (sgug_http_status(r) != 200) {
		seterr(err, errlen,
		    "runner-registration returned %d; the token is probably "
		    "expired or for a different org", sgug_http_status(r));
		goto out;
	}

	body = sgug_http_body(r, &body_len);
	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL) {
		seterr(err, errlen, "runner-registration returned non-JSON");
		goto out;
	}

	root = sgug_json_root(doc);
	sgug_snprintf(tenant_url, tenant_len, "%s",
	    sgug_json_string(sgug_json_get(root, "url"), ""));
	tok = sgug_json_string(sgug_json_get(root, "token"), NULL);
	*use_v2 = sgug_json_bool(sgug_json_get(root, "use_v2_flow"), 0);

	if (tenant_url[0] == '\0' || tok == NULL) {
		seterr(err, errlen,
		    "runner-registration response missing url or token");
		goto out;
	}

	*bearer = strdup(tok);
	rc = *bearer != NULL ? 0 : -1;

out:
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	sgug_jsonw_free(w);
	return rc;
}

/* Step 2: resolve the runner group name to a pool id. */
static int
resolve_pool(sgug_http_client *http, const char *tenant, const char *bearer,
    const char *group, int64_t *pool_id, char *pool_name, size_t namelen,
    char *err, size_t errlen)
{
	char url[SGUG_MAX_URL];
	char auth[2048];
	const char *headers[2];
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *vals;
	const char *body;
	size_t body_len, i, n;
	int rc = -1;

	sgug_snprintf(url, sizeof(url), "%s_apis/distributedtask/pools", tenant);
	sgug_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
	headers[0] = auth;
	headers[1] = ACCEPT_POOLS;

	if (sgug_http_request(http, "GET", url, headers, 2, NULL, 0, 30000,
	    &r) != 0) {
		seterr(err, errlen, "pools: %s", sgug_http_last_error());
		return -1;
	}
	if (sgug_http_status(r) != 200) {
		seterr(err, errlen, "pools returned %d", sgug_http_status(r));
		goto out;
	}

	body = sgug_http_body(r, &body_len);
	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL)
		goto out;

	vals = sgug_json_get(sgug_json_root(doc), "value");
	n = sgug_json_len(vals);

	for (i = 0; i < n; i++) {
		const sgug_json *p = sgug_json_at(vals, i);
		const char *name = sgug_json_string(sgug_json_get(p, "name"), "");

		/* Hosted pools belong to GitHub's own runners; a self-hosted
		 * agent cannot join one. */
		if (sgug_json_bool(sgug_json_get(p, "isHosted"), 0))
			continue;

		if (group != NULL) {
			if (strcmp(name, group) != 0)
				continue;
		} else if (!sgug_json_bool(sgug_json_get(p, "isInternal"), 0)) {
			/* No group asked for: take Default, which is the pool
			 * flagged internal. */
			continue;
		}

		*pool_id = sgug_json_int(sgug_json_get(p, "id"), 0);
		sgug_snprintf(pool_name, namelen, "%s", name);
		rc = 0;
		goto out;
	}

	if (group != NULL)
		seterr(err, errlen, "no runner group named \"%s\"", group);
	else
		seterr(err, errlen, "no default runner group found");

out:
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	return rc;
}

static void
emit_label(sgug_jsonw *w, const char *name, const char *type)
{
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "id");
	sgug_jsonw_int(w, 0);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, name);
	sgug_jsonw_key(w, "type");
	sgug_jsonw_str(w, type);
	sgug_jsonw_obj_end(w);
}

/* Step 3: create the agent, carrying our public key. */
static int
add_agent(sgug_http_client *http, const sgug_register_opts *opts,
    const char *tenant, const char *bearer, int64_t pool_id,
    const sgug_rsa *key, int64_t existing_id, sgug_config *cfg,
    char *err, size_t errlen)
{
	char url[SGUG_MAX_URL];
	char auth[2048];
	char modulus[512], exponent[32], created[40];
	const char *headers[3];
	sgug_jsonw *w = NULL;
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *root, *props;
	const char *body;
	size_t body_len, i;
	int rc = -1;

	if (sgug_rsa_public_modulus_b64(key, modulus, sizeof(modulus)) < 0 ||
	    sgug_rsa_public_exponent_b64(key, exponent, sizeof(exponent)) < 0) {
		seterr(err, errlen, "cannot encode public key");
		return -1;
	}

	sgug_format_iso8601(sgug_http_now(http), created, sizeof(created));

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);

	sgug_jsonw_key(w, "authorization");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "publicKey");
	sgug_jsonw_obj_begin(w);
	/* Standard base64 of the raw big-endian integer bytes, not DER. */
	sgug_jsonw_key(w, "exponent");
	sgug_jsonw_str(w, exponent);
	sgug_jsonw_key(w, "modulus");
	sgug_jsonw_str(w, modulus);
	sgug_jsonw_obj_end(w);
	sgug_jsonw_obj_end(w);

	sgug_jsonw_key(w, "labels");
	sgug_jsonw_arr_begin(w);
	/*
	 * The system labels are what ordinary workflow YAML matches on. X64 is
	 * untrue: GitHub's system vocabulary has no MIPS value, and claiming it
	 * is what makes `runs-on: [self-hosted, linux, x64]` schedulable here.
	 * The user labels below carry the truth.
	 */
	emit_label(w, "self-hosted", "system");
	emit_label(w, "Linux", "system");
	emit_label(w, "X64", "system");
	for (i = 0; i < opts->nlabels; i++)
		emit_label(w, opts->labels[i], "user");
	sgug_jsonw_arr_end(w);

	sgug_jsonw_key(w, "maxParallelism");
	sgug_jsonw_int(w, 1);
	sgug_jsonw_key(w, "id");
	sgug_jsonw_int(w, existing_id);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, opts->runner_name);
	sgug_jsonw_key(w, "version");
	sgug_jsonw_str(w, SGUG_RUNNER_VERSION);
	sgug_jsonw_key(w, "osDescription");
	sgug_jsonw_str(w, "IRIX 6.5 mips");
	sgug_jsonw_key(w, "provisioningState");
	sgug_jsonw_str(w, "Provisioned");
	sgug_jsonw_key(w, "createdOn");
	sgug_jsonw_str(w, created);
	sgug_jsonw_key(w, "ephemeral");
	sgug_jsonw_bool(w, 0);
	/* Without this the service sends AgentRefreshMessage telling us to
	 * download a .NET tarball we could not execute. */
	sgug_jsonw_key(w, "disableUpdate");
	sgug_jsonw_bool(w, 1);
	sgug_jsonw_key(w, "properties");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_obj_end(w);

	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	sgug_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
	headers[0] = auth;
	headers[1] = ACCEPT_AGENTS;
	headers[2] = CT_AGENTS;

	if (existing_id != 0) {
		sgug_snprintf(url, sizeof(url),
		    "%s_apis/distributedtask/pools/%ld/agents/%ld", tenant,
		    (long)pool_id, (long)existing_id);
	} else {
		sgug_snprintf(url, sizeof(url),
		    "%s_apis/distributedtask/pools/%ld/agents", tenant,
		    (long)pool_id);
	}

	if (sgug_http_request(http, existing_id != 0 ? "PUT" : "POST", url,
	    headers, 3, body, body_len, 30000, &r) != 0) {
		seterr(err, errlen, "agent add: %s", sgug_http_last_error());
		goto out;
	}

	if (sgug_http_status(r) != 200 && sgug_http_status(r) != 201) {
		const char *rb = sgug_http_body(r, NULL);

		seterr(err, errlen, "agent add returned %d", sgug_http_status(r));
		if (err != NULL && errlen > 0 && rb != NULL && *rb != '\0') {
			size_t used = strlen(err);

			sgug_snprintf(err + used, errlen - used, ": %.300s", rb);
		}
		goto out;
	}

	body = sgug_http_body(r, &body_len);
	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc == NULL)
		goto out;

	root = sgug_json_root(doc);
	cfg->agent_id = sgug_json_int(sgug_json_get(root, "id"), 0);
	sgug_snprintf(cfg->client_id, sizeof(cfg->client_id), "%s",
	    sgug_json_string(sgug_json_path(root, "authorization.clientId"), ""));
	sgug_snprintf(cfg->auth_url, sizeof(cfg->auth_url), "%s",
	    sgug_json_string(
	    sgug_json_path(root, "authorization.authorizationUrl"), ""));

	/*
	 * Properties decide the rest of the runner's life: whether assertions
	 * are PS256, and whether messages come from the broker rather than the
	 * pool. Values may be plain scalars or {"$type":...,"$value":...}, so
	 * they are read through the string-tolerant accessors.
	 */
	props = sgug_json_get(root, "properties");
	cfg->require_fips = sgug_json_bool(
	    sgug_json_get(props, "RequireFipsCryptography"), 0);
	cfg->use_v2_flow = sgug_json_bool(sgug_json_get(props, "UseV2Flow"), 0);
	sgug_snprintf(cfg->server_url_v2, sizeof(cfg->server_url_v2), "%s",
	    sgug_json_string(sgug_json_get(props, "ServerUrlV2"), ""));

	if (cfg->client_id[0] == '\0' || cfg->auth_url[0] == '\0') {
		seterr(err, errlen,
		    "agent add response missing clientId or authorizationUrl");
		goto out;
	}

	rc = 0;

out:
	sgug_json_free(doc);
	sgug_http_resp_free(r);
	sgug_jsonw_free(w);
	return rc;
}

/* Looks for an existing agent of this name so we can replace it in place. */
static int64_t
find_agent(sgug_http_client *http, const char *tenant, const char *bearer,
    int64_t pool_id, const char *name)
{
	char url[SGUG_MAX_URL];
	char auth[2048];
	const char *headers[2];
	sgug_http_resp *r = NULL;
	sgug_json_doc *doc = NULL;
	const sgug_json *vals;
	const char *body;
	size_t body_len;
	int64_t id = 0;

	sgug_snprintf(url, sizeof(url),
	    "%s_apis/distributedtask/pools/%ld/agents?agentName=%s", tenant,
	    (long)pool_id, name);
	sgug_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
	headers[0] = auth;
	headers[1] = ACCEPT_AGENTS;

	if (sgug_http_request(http, "GET", url, headers, 2, NULL, 0, 30000,
	    &r) != 0)
		return 0;

	if (sgug_http_status(r) == 200) {
		body = sgug_http_body(r, &body_len);
		doc = sgug_json_parse(body, body_len, NULL, 0);
		if (doc != NULL) {
			vals = sgug_json_get(sgug_json_root(doc), "value");
			if (sgug_json_len(vals) > 0)
				id = sgug_json_int(
				    sgug_json_get(sgug_json_at(vals, 0), "id"), 0);
		}
	}

	sgug_json_free(doc);
	sgug_http_resp_free(r);
	return id;
}

int
sgug_register(sgug_http_client *http, const sgug_register_opts *opts,
    const char *dir, sgug_config *out, char *err, size_t errlen)
{
	char tenant[SGUG_MAX_URL];
	char keypath[SGUG_MAX_URL];
	char *bearer = NULL;
	sgug_rsa *key = NULL;
	int64_t existing = 0;
	int use_v2 = 0;
	int rc = -1;

	memset(out, 0, sizeof(*out));

	if (runner_registration(http, opts, tenant, sizeof(tenant), &bearer,
	    &use_v2, err, errlen) != 0)
		return -1;

	sgug_snprintf(out->server_url, sizeof(out->server_url), "%s", tenant);
	sgug_snprintf(out->github_url, sizeof(out->github_url), "%s",
	    opts->github_url);
	sgug_snprintf(out->agent_name, sizeof(out->agent_name), "%s",
	    opts->runner_name);
	sgug_snprintf(out->work_folder, sizeof(out->work_folder), "%s",
	    opts->work_folder != NULL ? opts->work_folder : "_work");
	out->disable_update = 1;
	out->use_v2_flow = use_v2;

	if (resolve_pool(http, tenant, bearer, opts->runner_group, &out->pool_id,
	    out->pool_name, sizeof(out->pool_name), err, errlen) != 0)
		goto out;

	key = sgug_rsa_generate();
	if (key == NULL) {
		seterr(err, errlen, "RSA key generation failed");
		goto out;
	}

	existing = find_agent(http, tenant, bearer, out->pool_id,
	    opts->runner_name);
	if (existing != 0 && !opts->replace) {
		seterr(err, errlen,
		    "a runner named \"%s\" already exists in this group; pass "
		    "--replace to take it over", opts->runner_name);
		goto out;
	}

	if (add_agent(http, opts, tenant, bearer, out->pool_id, key, existing,
	    out, err, errlen) != 0)
		goto out;

	/* Written only after the service has accepted the public key, so a
	 * failed registration leaves no stale private key behind. */
	sgug_config_path(dir, ".rsakey", keypath, sizeof(keypath));
	if (sgug_rsa_save(key, keypath) != 0) {
		seterr(err, errlen, "cannot write %s", keypath);
		goto out;
	}

	if (sgug_config_save(out, dir) != 0) {
		seterr(err, errlen, "cannot write configuration to %s", dir);
		goto out;
	}

	rc = 0;

out:
	if (bearer != NULL) {
		explicit_bzero(bearer, strlen(bearer));
		free(bearer);
	}
	sgug_rsa_free(key);
	return rc;
}

int
sgug_unregister(sgug_http_client *http, const char *dir,
    const char *removal_token, char *err, size_t errlen)
{
	sgug_config cfg;
	sgug_register_opts opts;
	char tenant[SGUG_MAX_URL];
	char url[SGUG_MAX_URL];
	char auth[2048];
	const char *headers[2];
	char *bearer = NULL;
	sgug_http_resp *r = NULL;
	int use_v2 = 0;
	int rc = -1;

	if (sgug_config_load(&cfg, dir) != 0) {
		seterr(err, errlen, "no runner configured in %s", dir);
		return -1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.github_url = cfg.github_url;
	opts.reg_token = removal_token;

	if (runner_registration(http, &opts, tenant, sizeof(tenant), &bearer,
	    &use_v2, err, errlen) != 0)
		return -1;

	sgug_snprintf(url, sizeof(url),
	    "%s_apis/distributedtask/pools/%ld/agents/%ld", tenant,
	    (long)cfg.pool_id, (long)cfg.agent_id);
	sgug_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
	headers[0] = auth;
	headers[1] = ACCEPT_AGENTS;

	if (sgug_http_request(http, "DELETE", url, headers, 2, NULL, 0, 30000,
	    &r) != 0) {
		seterr(err, errlen, "delete agent: %s", sgug_http_last_error());
		goto out;
	}

	/* 404 means it is already gone, which is the desired end state. */
	if (sgug_http_status(r) >= 300 && sgug_http_status(r) != 404) {
		seterr(err, errlen, "delete agent returned %d",
		    sgug_http_status(r));
		goto out;
	}

	rc = sgug_config_remove(dir);

out:
	if (bearer != NULL) {
		explicit_bzero(bearer, strlen(bearer));
		free(bearer);
	}
	sgug_http_resp_free(r);
	return rc;
}
