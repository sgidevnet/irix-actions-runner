#include "compat/irix.h"
#include "exec/runjob.h"
#include "json/json.h"
#include "net/http.h"
#include "proto/config.h"
#include "proto/listener.h"
#include "proto/oauth.h"
#include "proto/register.h"
#include "version.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
usage(FILE *to, int rc)
{
	fputs("usage: runner <command> [options]\n"
	      "\n"
	      "  configure --url URL --token TOKEN [options]\n"
	      "      --url URL          https://github.com/org or .../org/repo\n"
	      "      --token TOKEN      registration token, from the UI or API\n"
	      "      --name NAME        runner name, default the hostname\n"
	      "      --runnergroup NAME runner group, default Default\n"
	      "      --labels A,B,C     extra labels beyond irix,mips,mips-n32\n"
	      "      --work DIR         work folder, default _work\n"
	      "      --replace          take over an existing runner of this name\n"
	      "\n"
	      "  run [--verbose|--trace]\n"
	      "      listen for jobs; this is what makes the runner show Online\n"
	      "\n"
	      "  remove --token TOKEN   deregister and delete local configuration\n"
	      "  status                 show the configured runner\n"
	      "  selftest [host]        check TLS, certificates, HTTP and clock\n"
	      "  version                print the version of this runner\n"
	      "  help                   print this message\n",
	      to);
	return rc;
}

static const char *
arg_after(int argc, char **argv, int *i, const char *what)
{
	if (*i + 1 >= argc) {
		fprintf(stderr, "%s needs a value\n", what);
		exit(2);
	}
	(*i)++;
	return argv[*i];
}

/* Splits "a,b,c" in place. Returns the count. */
static size_t
split_labels(char *s, const char **out, size_t max)
{
	size_t n = 0;

	while (*s != '\0' && n < max) {
		char *comma = strchr(s, ',');

		if (comma != NULL)
			*comma = '\0';
		if (*s != '\0')
			out[n++] = s;
		if (comma == NULL)
			break;
		s = comma + 1;
	}
	return n;
}

static int
cmd_configure(int argc, char **argv)
{
	sgug_register_opts opts;
	sgug_config cfg;
	sgug_http_client *http;
	char hostname[128];
	char err[512];
	const char *labels[32];
	size_t nlabels = 0;
	int i, rc;

	memset(&opts, 0, sizeof(opts));
	opts.work_folder = "_work";

	/* Always present, so a workflow can target this machine on purpose
	 * rather than relying on the system labels, which claim X64. */
	labels[nlabels++] = "irix";
	labels[nlabels++] = "mips";
	labels[nlabels++] = "mips-n32";

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--url") == 0)
			opts.github_url = arg_after(argc, argv, &i, "--url");
		else if (strcmp(argv[i], "--token") == 0)
			opts.reg_token = arg_after(argc, argv, &i, "--token");
		else if (strcmp(argv[i], "--name") == 0)
			opts.runner_name = arg_after(argc, argv, &i, "--name");
		else if (strcmp(argv[i], "--runnergroup") == 0)
			opts.runner_group = arg_after(argc, argv, &i, "--runnergroup");
		else if (strcmp(argv[i], "--work") == 0)
			opts.work_folder = arg_after(argc, argv, &i, "--work");
		else if (strcmp(argv[i], "--replace") == 0)
			opts.replace = 1;
		else if (strcmp(argv[i], "--labels") == 0) {
			char *v = (char *)arg_after(argc, argv, &i, "--labels");

			nlabels += split_labels(v, labels + nlabels,
			    sizeof(labels) / sizeof(labels[0]) - nlabels);
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if (opts.github_url == NULL || opts.reg_token == NULL) {
		fprintf(stderr, "--url and --token are required\n");
		return 2;
	}

	if (opts.runner_name == NULL) {
		if (gethostname(hostname, sizeof(hostname)) != 0)
			sgug_snprintf(hostname, sizeof(hostname), "irix-runner");
		hostname[sizeof(hostname) - 1] = '\0';
		opts.runner_name = hostname;
	}

	opts.labels = labels;
	opts.nlabels = nlabels;

	if (sgug_config_exists(".") && !opts.replace) {
		fprintf(stderr, "this directory already holds a configured "
		    "runner; remove it first or pass --replace\n");
		return 1;
	}

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		return 1;
	}

	err[0] = '\0';
	rc = sgug_register(http, &opts, ".", &cfg, err, sizeof(err));
	if (rc != 0) {
		fprintf(stderr, "registration failed: %s\n", err);
		sgug_http_client_free(http);
		return 1;
	}

	printf("runner    %s\n", cfg.agent_name);
	printf("group     %s (pool %ld)\n", cfg.pool_name, (long)cfg.pool_id);
	printf("agent id  %ld\n", (long)cfg.agent_id);
	printf("flow      %s\n", cfg.use_v2_flow ? "v2 broker" : "v1 pool");
	printf("signing   %s\n", cfg.require_fips ? "PS256" : "RS256");
	printf("\nconfigured. run `runner run` to start listening.\n");

	sgug_http_client_free(http);
	return 0;
}

static int
cmd_remove(int argc, char **argv)
{
	sgug_http_client *http;
	const char *token = NULL;
	char err[512];
	int i, rc;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--token") == 0)
			token = arg_after(argc, argv, &i, "--token");
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if (token == NULL) {
		fprintf(stderr, "--token is required; the OAuth credentials "
		    "cannot authorise their own deletion\n");
		return 2;
	}

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		return 1;
	}

	err[0] = '\0';
	rc = sgug_unregister(http, ".", token, err, sizeof(err));
	if (rc != 0)
		fprintf(stderr, "removal failed: %s\n", err);
	else
		printf("runner removed\n");

	sgug_http_client_free(http);
	return rc == 0 ? 0 : 1;
}

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* Lets a blocked long poll surface a SIGTERM instead of running to completion. */
static int
should_abort(void *ctx)
{
	(void)ctx;
	return stop_requested != 0;
}

struct dispatch_ctx {
	sgug_http_client *http;
	const sgug_config *cfg;
	int seen;
};

static int
dispatch_message(void *ctx, const sgug_message *msg)
{
	struct dispatch_ctx *d = ctx;
	char err[512];

	d->seen++;

	if (getenv("SGUG_DUMP_MESSAGES") != NULL)
		printf("          body: %.*s\n", (int)msg->body_len, msg->body);

	if (strcmp(msg->type, "PipelineAgentJobRequest") == 0 ||
	    strcmp(msg->type, "RunnerJobRequest") == 0) {
		err[0] = '\0';
		if (sgug_run_job(d->http, d->cfg, msg->body, msg->body_len,
		    &stop_requested, err, sizeof(err)) != 0)
			fprintf(stderr, "job failed to report: %s\n", err);
	}

	fflush(stdout);
	return 0;
}

static int
cmd_run(int argc, char **argv)
{
	sgug_listener_opts opts;
	sgug_config cfg;
	sgug_http_client *http;
	sgug_oauth *oauth;
	sgug_rsa *key;
	char keypath[SGUG_MAX_URL];
	char err[512];
	struct dispatch_ctx dctx;
	int verbose = 0, i, rc;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--verbose") == 0)
			verbose = 1;
		else if (strcmp(argv[i], "--trace") == 0)
			verbose = 2;
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if (sgug_config_load(&cfg, ".") != 0) {
		fprintf(stderr, "no runner configured in this directory; run "
		    "`runner configure` first\n");
		return 1;
	}

	sgug_config_path(".", ".rsakey", keypath, sizeof(keypath));
	key = sgug_rsa_load(keypath);
	if (key == NULL) {
		fprintf(stderr, "cannot load %s\n", keypath);
		return 1;
	}

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		sgug_rsa_free(key);
		return 1;
	}

	sgug_http_set_abort_check(http, should_abort, NULL);

	oauth = sgug_oauth_new(http, &cfg, key);
	if (oauth == NULL) {
		sgug_http_client_free(http);
		sgug_rsa_free(key);
		return 1;
	}

	/*
	 * sigaction without SA_RESTART, deliberately. signal() installs
	 * restarting handlers, which means a SIGTERM arriving during a long
	 * poll is not seen until the poll returns, up to two minutes later, and
	 * the session is never torn down so the runner lingers Online. Without
	 * SA_RESTART the read fails with EINTR, the poll returns, and the loop
	 * exits within a second.
	 *
	 * SIGINT and SIGTERM only. Never touch 47 or 48; IRIX libpthread
	 * reserves them and handling them corrupts the threading runtime.
	 */
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_signal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}
	signal(SIGPIPE, SIG_IGN);

	memset(&opts, 0, sizeof(opts));
	opts.http = http;
	opts.cfg = &cfg;
	opts.oauth = oauth;
	opts.key = key;
	dctx.http = http;
	dctx.cfg = &cfg;
	dctx.seen = 0;

	opts.on_message = dispatch_message;
	opts.ctx = &dctx;
	opts.stop = &stop_requested;
	opts.verbose = verbose;

	err[0] = '\0';
	rc = sgug_listen(&opts, err, sizeof(err));
	if (rc != 0 && err[0] != '\0')
		fprintf(stderr, "%s\n", err);

	printf("\nstopped after %d message(s)\n", dctx.seen);

	sgug_oauth_free(oauth);
	sgug_http_client_free(http);
	sgug_rsa_free(key);
	return rc == 0 ? 0 : 1;
}

static int
cmd_status(void)
{
	sgug_config cfg;

	if (sgug_config_load(&cfg, ".") != 0) {
		fprintf(stderr, "no runner configured in this directory\n");
		return 1;
	}

	printf("runner    %s\n", cfg.agent_name);
	printf("url       %s\n", cfg.github_url);
	printf("group     %s (pool %ld)\n", cfg.pool_name, (long)cfg.pool_id);
	printf("agent id  %ld\n", (long)cfg.agent_id);
	printf("tenant    %s\n", cfg.server_url);
	printf("flow      %s\n", cfg.use_v2_flow ? "v2 broker" : "v1 pool");
	if (cfg.server_url_v2[0] != '\0')
		printf("broker    %s\n", cfg.server_url_v2);
	printf("signing   %s\n", cfg.require_fips ? "PS256" : "RS256");
	printf("work      %s\n", cfg.work_folder);
	return 0;
}

/*
 * Everything the runner needs from the network, end to end, with the failure
 * modes separated. On a freshly imaged IRIX box the usual causes are an expired
 * CA bundle and an undisciplined clock, and both otherwise surface much later
 * as an unexplained 401 during OAuth.
 */
static int
selftest(const char *host)
{
	sgug_http_client *c;
	sgug_http_resp *r;
	sgug_json_doc *doc;
	char url[512];
	const char *server, *date, *body;
	sgug_time_t skew;
	size_t body_len;
	int rc = 1;

	printf("host      %s\n", host);

	c = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (c == NULL) {
		fprintf(stderr, "client: %s\n", sgug_http_last_error());
		return 1;
	}

	sgug_snprintf(url, sizeof(url), "https://%s/", host);

	if (sgug_http_request(c, "GET", url, NULL, 0, NULL, 0, 30000, &r) != 0) {
		fprintf(stderr, "request: %s\n", sgug_http_last_error());
		sgug_http_client_free(c);
		return 1;
	}

	body = sgug_http_body(r, &body_len);
	server = sgug_http_header(r, "Server");
	date = sgug_http_header(r, "Date");

	printf("status    %d\n", sgug_http_status(r));
	printf("server    %s\n", server != NULL ? server : "(none)");
	printf("body      %lu bytes\n", (unsigned long)body_len);

	if (date == NULL) {
		fprintf(stderr, "no Date header, cannot measure clock\n");
		goto out;
	}

	skew = sgug_http_skew(c);
	printf("clock     skew %+lds vs server\n", (long)skew);
	if (skew > 60 || skew < -60) {
		printf("warning   clock is off by more than a minute; the runner\n");
		printf("          will compensate, but enable NTP on this machine\n");
	}

	doc = sgug_json_parse(body, body_len, NULL, 0);
	if (doc != NULL) {
		printf("json      parsed, %lu top-level keys\n",
		    (unsigned long)sgug_json_len(sgug_json_root(doc)));
		sgug_json_free(doc);
	} else if (body_len > 0) {
		printf("json      response was not JSON, which is fine here\n");
	}

	sgug_http_resp_free(r);
	r = NULL;
	if (sgug_http_request(c, "GET", url, NULL, 0, NULL, 0, 30000, &r) != 0) {
		fprintf(stderr, "second request: %s\n", sgug_http_last_error());
		goto out;
	}
	printf("keepalive second request returned %d\n", sgug_http_status(r));

	printf("\nselftest OK\n");
	rc = 0;

out:
	sgug_http_resp_free(r);
	sgug_http_client_free(c);
	return rc;
}

/*
 * Makes the release tarball work on a machine with no SGUG-RSE, where the
 * compiled-in bundle path does not exist. tls.c already consults SSL_CERT_FILE,
 * so pointing that at the shipped copy is the whole fix.
 *
 * Skipped when argv[0] carries no directory, since a PATH lookup would have to
 * be repeated here to find the real one.
 */
static void
adopt_adjacent_ca_bundle(const char *argv0)
{
	char path[512];
	const char *slash;
	size_t dirlen;

	if (getenv("SSL_CERT_FILE") != NULL || argv0 == NULL)
		return;

	slash = strrchr(argv0, '/');
	if (slash == NULL)
		return;

	dirlen = (size_t)(slash - argv0);
	if (dirlen + sizeof("/cert.pem") >= sizeof(path))
		return;

	memcpy(path, argv0, dirlen);
	sgug_snprintf(path + dirlen, sizeof(path) - dirlen, "/cert.pem");

	if (access(path, R_OK) == 0)
		setenv("SSL_CERT_FILE", path, 1);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		return usage(stderr, 2);

	adopt_adjacent_ca_bundle(argv[0]);

	if (strcmp(argv[1], "version") == 0 ||
	    strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
		printf("irix-actions-runner %s\n", SGUG_PROJECT_VERSION);
		printf("protocol %s\n", SGUG_RUNNER_VERSION);
		return 0;
	}
	if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)
		return usage(stdout, 0);
	if (strcmp(argv[1], "selftest") == 0)
		return selftest(argc > 2 ? argv[2] : "api.github.com");
	if (strcmp(argv[1], "configure") == 0)
		return cmd_configure(argc, argv);
	if (strcmp(argv[1], "remove") == 0)
		return cmd_remove(argc, argv);
	if (strcmp(argv[1], "run") == 0)
		return cmd_run(argc, argv);
	if (strcmp(argv[1], "status") == 0)
		return cmd_status();

	fprintf(stderr, "unknown command: %s\n\n", argv[1]);
	return usage(stderr, 2);
}
