#include "compat/irix.h"
#include "exec/runjob.h"
#include "json/json.h"
#include "net/http.h"
#include "proto/config.h"
#include "proto/listener.h"
#include "proto/oauth.h"
#include "proto/register.h"
#include "serve/serve.h"
#include "version.h"

#include <sys/stat.h>

#include <errno.h>
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
	      "      --count N          register N identities, not one; adds\n"
	      "                         an `emulated` label, excludes --name\n"
	      "                         and --work\n"
	      "      --name-prefix P    with --count: P-0, P-1, ... name\n"
	      "                         both the directory and the runner\n"
	      "\n"
	      "  run [--dir DIR] [--verbose|--trace]\n"
	      "      listen for jobs; this is what makes the runner show Online\n"
	      "      --dir DIR          runner directory, default .\n"
	      "\n"
	      "  serve [--count N --name-prefix P] [--verbose|--trace]\n"
	      "      one listener per identity, each job in its own container\n"
	      "      Linux only; on IRIX use `run`\n"
	      "      --count N          serve N identities, not one\n"
	      "      --name-prefix P    directories P-0, P-1, ...\n"
	      "      --image NAME       worker image, irix-worker:latest\n"
	      "      --job-timeout SECS wall clock per job, default 14400\n"
	      "\n"
	      "  execjob --message FILE --name NAME --work DIR [options]\n"
	      "      run one job from a message file; needs no configuration\n"
	      "      --message FILE     the job message as received\n"
	      "      --name NAME        runner name the job sees\n"
	      "      --work DIR         work folder, must be absolute\n"
	      "      --cancel-file PATH abort when this file's mtime changes\n"
	      "\n"
	      "  remove --token TOKEN [--count N --name-prefix P]\n"
	      "      deregister and delete local configuration\n"
	      "  status [--dir DIR]     show the configured runner\n"
	      "  selftest [host]        check TLS, certificates, HTTP, clock\n"
	      "                         and, on Linux, the docker socket\n"
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

static int
num_after(int argc, char **argv, int *i, const char *what, long max)
{
	const char *v = arg_after(argc, argv, i, what);
	char *end;
	long n;

	n = strtol(v, &end, 10);
	if (*v == '\0' || *end != '\0' || n < 1 || n > max) {
		fprintf(stderr, "%s must be a number from 1 to %ld\n", what,
		    max);
		exit(2);
	}
	return (int)n;
}

/* A truncated "-N" suffix would give two identities one directory, and the
 * second registration would replace the first. 16 covers "-63/_work". */
static void
check_prefix(const char *prefix)
{
	if (prefix == NULL || strlen(prefix) + 16 <= SGUG_MAX_NAME)
		return;
	fprintf(stderr, "--name-prefix is too long\n");
	exit(2);
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
	char dir[SGUG_MAX_NAME];
	char work[SGUG_MAX_NAME];
	char cwd[SGUG_MAX_NAME];
	char err[512];
	const char *labels[32];
	const char *prefix = NULL;
	size_t nlabels = 0;
	int count = 0, work_set = 0;
	int i, n;

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
		else if (strcmp(argv[i], "--work") == 0) {
			opts.work_folder = arg_after(argc, argv, &i, "--work");
			work_set = 1;
		} else if (strcmp(argv[i], "--replace") == 0)
			opts.replace = 1;
		else if (strcmp(argv[i], "--count") == 0)
			count = num_after(argc, argv, &i, "--count",
			    SGUG_MAX_IDENTITIES);
		else if (strcmp(argv[i], "--name-prefix") == 0)
			prefix = arg_after(argc, argv, &i, "--name-prefix");
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

	if (count > 0 && work_set) {
		fprintf(stderr, "--work cannot be combined with --count; each "
		    "identity gets its own work folder\n");
		return 2;
	}

	if ((count > 0) != (prefix != NULL)) {
		fprintf(stderr, "--count and --name-prefix go together\n");
		return 2;
	}

	if (count > 0 && opts.runner_name != NULL) {
		fprintf(stderr, "--name and --count are exclusive; each "
		    "identity is named after --name-prefix\n");
		return 2;
	}

	check_prefix(prefix);

	if (count == 0 && opts.runner_name == NULL) {
		if (gethostname(hostname, sizeof(hostname)) != 0)
			sgug_snprintf(hostname, sizeof(hostname), "irix-runner");
		hostname[sizeof(hostname) - 1] = '\0';
		opts.runner_name = hostname;
	}

	if (count > 0) {
		if (nlabels == sizeof(labels) / sizeof(labels[0])) {
			fprintf(stderr, "too many labels\n");
			return 2;
		}
		labels[nlabels++] = "emulated";
	}

	opts.labels = labels;
	opts.nlabels = nlabels;

	if (count > 0 && getcwd(cwd, sizeof(cwd)) == NULL) {
		fprintf(stderr, "cannot resolve the current directory: %s\n",
		    strerror(errno));
		return 1;
	}

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		return 1;
	}

	for (n = 0; n < (count > 0 ? count : 1); n++) {
		const char *d = ".";

		if (count > 0) {
			sgug_snprintf(dir, sizeof(dir), "%s-%d", prefix, n);
			d = dir;
			opts.runner_name = dir;
			if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
				fprintf(stderr, "%s: %s\n", dir,
				    strerror(errno));
				sgug_http_client_free(http);
				return 1;
			}
			/*
			 * Absolute, and one each: runjob keys the workspace on
			 * the repo name alone, so identities sharing a work
			 * folder would share a checkout, and a relative one
			 * would follow the cwd of `runner run` instead of the
			 * identity.
			 */
			if (sgug_snprintf(work, sizeof(work), "%s/%s/_work",
			    cwd, dir) != (int)(strlen(cwd) + strlen(dir) + 7)) {
				fprintf(stderr, "%s: work folder path is too "
				    "long\n", dir);
				sgug_http_client_free(http);
				return 1;
			}
			opts.work_folder = work;
		}

		if (sgug_config_exists(d) && !opts.replace) {
			fprintf(stderr, "%s already holds a configured runner; "
			    "remove it first or pass --replace\n", d);
			sgug_http_client_free(http);
			return 1;
		}

		/* Stops at the first failure, unlike cmd_remove. Recover by
		 * removing the identities that did register: --replace would
		 * rotate the RSA key of every one, and a listener already
		 * running against an old key cannot unwrap its session. */
		err[0] = '\0';
		if (sgug_register(http, &opts, d, &cfg, err,
		    sizeof(err)) != 0) {
			fprintf(stderr, "registration failed in %s: %s\n", d,
			    err);
			sgug_http_client_free(http);
			return 1;
		}

		if (count > 0) {
			char id[24];

			sgug_i64toa(cfg.agent_id, id, sizeof(id));
			printf("%-24s agent %s\n", dir, id);
			continue;
		}

		printf("runner    %s\n", cfg.agent_name);
		printf("group     %s (pool %ld)\n", cfg.pool_name,
		    (long)cfg.pool_id);
		printf("agent id  %ld\n", (long)cfg.agent_id);
		printf("flow      %s\n",
		    cfg.use_v2_flow ? "v2 broker" : "v1 pool");
		printf("signing   %s\n",
		    cfg.require_fips ? "PS256" : "RS256");
	}

	if (count > 0)
		printf("\n%d identities configured. start one `runner run "
		    "--dir NAME` per identity.\n", count);
	else
		printf("\nconfigured. run `runner run` to start listening.\n");

	sgug_http_client_free(http);
	return 0;
}

static int
cmd_remove(int argc, char **argv)
{
	sgug_http_client *http;
	const char *token = NULL;
	const char *prefix = NULL;
	char dir[SGUG_MAX_NAME];
	char err[512];
	int count = 0;
	int i, n, rc = 0;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--token") == 0)
			token = arg_after(argc, argv, &i, "--token");
		else if (strcmp(argv[i], "--count") == 0)
			count = num_after(argc, argv, &i, "--count",
			    SGUG_MAX_IDENTITIES);
		else if (strcmp(argv[i], "--name-prefix") == 0)
			prefix = arg_after(argc, argv, &i, "--name-prefix");
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

	if ((count > 0) != (prefix != NULL)) {
		fprintf(stderr, "--count and --name-prefix go together\n");
		return 2;
	}
	check_prefix(prefix);

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		return 1;
	}

	/* Keeps going after a failure, so one dead identity does not leave the
	 * rest registered. */
	for (n = 0; n < (count > 0 ? count : 1); n++) {
		const char *d = ".";

		if (count > 0) {
			sgug_snprintf(dir, sizeof(dir), "%s-%d", prefix, n);
			d = dir;
		}

		err[0] = '\0';
		if (sgug_unregister(http, d, token, err, sizeof(err)) != 0) {
			fprintf(stderr, "removal failed in %s: %s\n", d, err);
			rc = 1;
		} else if (count > 0) {
			printf("runner removed from %s\n", d);
		} else {
			printf("runner removed\n");
		}
	}

	sgug_http_client_free(http);
	return rc;
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
	sgug_job_runner run_job;
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
		if (d->run_job(d->http, d->cfg, msg->body, msg->body_len,
		    &stop_requested, err, sizeof(err)) != 0)
			fprintf(stderr, "job failed to report: %s\n", err);
	}

	fflush(stdout);
	return 0;
}

/* Also serve's per-child body; hence the executor parameter. */
static int
run_dir(const char *dir, int verbose, sgug_job_runner run)
{
	sgug_listener_opts opts;
	sgug_config cfg;
	sgug_http_client *http;
	sgug_oauth *oauth;
	sgug_rsa *key;
	char keypath[SGUG_MAX_URL];
	char err[512];
	struct dispatch_ctx dctx;
	int rc;

	if (sgug_config_load(&cfg, dir) != 0) {
		fprintf(stderr, "no runner configured in %s; run "
		    "`runner configure` first\n", dir);
		return 1;
	}

	/* runjob resolves a relative work folder against the cwd, so without
	 * this `run --dir X` would put the workspace beside the caller. */
	if (strcmp(dir, ".") != 0 && cfg.work_folder[0] != '/') {
		char work[SGUG_MAX_NAME];

		if (sgug_snprintf(work, sizeof(work), "%s/%s", dir,
		    cfg.work_folder) != (int)(strlen(dir) +
		    strlen(cfg.work_folder) + 1)) {
			fprintf(stderr, "%s: work folder path is too long\n",
			    dir);
			return 1;
		}
		sgug_snprintf(cfg.work_folder, sizeof(cfg.work_folder), "%s",
		    work);
	}

	sgug_config_path(dir, ".rsakey", keypath, sizeof(keypath));
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
		sigset_t mask;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_signal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);

		/* serve blocks both across the fork; see serve.h. */
		sigemptyset(&mask);
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGTERM);
		sigprocmask(SIG_UNBLOCK, &mask, NULL);
	}
	signal(SIGPIPE, SIG_IGN);

	memset(&opts, 0, sizeof(opts));
	opts.http = http;
	opts.cfg = &cfg;
	opts.oauth = oauth;
	opts.key = key;
	dctx.http = http;
	dctx.cfg = &cfg;
	dctx.run_job = run;
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
cmd_run(int argc, char **argv)
{
	const char *dir = ".";
	int verbose = 0, i;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--verbose") == 0)
			verbose = 1;
		else if (strcmp(argv[i], "--trace") == 0)
			verbose = 2;
		else if (strcmp(argv[i], "--dir") == 0)
			dir = arg_after(argc, argv, &i, "--dir");
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	return run_dir(dir, verbose, sgug_run_job);
}

static int
cmd_serve(int argc, char **argv)
{
	sgug_serve_opts opts;
	int i;

	memset(&opts, 0, sizeof(opts));

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--count") == 0)
			opts.count = num_after(argc, argv, &i, "--count",
			    SGUG_MAX_IDENTITIES);
		else if (strcmp(argv[i], "--name-prefix") == 0)
			opts.name_prefix = arg_after(argc, argv, &i,
			    "--name-prefix");
		else if (strcmp(argv[i], "--image") == 0)
			opts.image = arg_after(argc, argv, &i, "--image");
		else if (strcmp(argv[i], "--job-timeout") == 0)
			opts.job_timeout = num_after(argc, argv, &i,
			    "--job-timeout", 86400);
		else if (strcmp(argv[i], "--verbose") == 0)
			opts.verbose = 1;
		else if (strcmp(argv[i], "--trace") == 0)
			opts.verbose = 2;
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if ((opts.count > 0) != (opts.name_prefix != NULL)) {
		fprintf(stderr, "--count and --name-prefix go together\n");
		return 2;
	}
	check_prefix(opts.name_prefix);

	return sgug_serve(&opts, run_dir);
}

/* step.c polls the abort flag every 500ms, so this is the added latency. */
#define CANCEL_POLL_SECS 5

static volatile sig_atomic_t cancel_requested;
static const char *cancel_path;
static sgug_time_t cancel_mtime;

/*
 * mtime, not the file appearing: on an NFS export IRIX caches the directory
 * lookup for acdirmax, 60 seconds by default, so a file created after the job
 * started stays invisible that long. Attribute caching still bounds how fresh
 * the mtime itself is.
 */
static void
on_cancel_alarm(int sig)
{
	struct stat st;
	int saved = errno;

	(void)sig;
	if (stat(cancel_path, &st) == 0 &&
	    (sgug_time_t)st.st_mtime != cancel_mtime)
		cancel_requested = 1;
	alarm(CANCEL_POLL_SECS);
	errno = saved;
}

static void
on_cancel_signal(int sig)
{
	(void)sig;
	cancel_requested = 1;
}

/*
 * cfg is a stack stub carrying the only two fields anything below
 * sgug_run_job reads. Token, service URLs and secrets are all in the message,
 * so a throwaway host never needs .runner, .credentials or .rsakey.
 */
static int
cmd_execjob(int argc, char **argv)
{
	sgug_config cfg;
	sgug_http_client *http = NULL;
	FILE *f = NULL;
	char *message = NULL;
	char err[512];
	const char *path = NULL, *name = NULL, *work = NULL;
	long len = 0;
	int i, rc = 1;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--message") == 0)
			path = arg_after(argc, argv, &i, "--message");
		else if (strcmp(argv[i], "--name") == 0)
			name = arg_after(argc, argv, &i, "--name");
		else if (strcmp(argv[i], "--work") == 0)
			work = arg_after(argc, argv, &i, "--work");
		else if (strcmp(argv[i], "--cancel-file") == 0)
			cancel_path = arg_after(argc, argv, &i,
			    "--cancel-file");
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if (path == NULL || name == NULL || work == NULL) {
		fprintf(stderr, "--message, --name and --work are required\n");
		return 2;
	}

	/* Relative resolves against getcwd inside sgug_run_job, which the
	 * caller does not control. */
	if (work[0] != '/') {
		fprintf(stderr, "--work must be an absolute path\n");
		return 2;
	}
	if (strlen(work) >= sizeof(cfg.work_folder)) {
		fprintf(stderr, "--work must be under %lu bytes\n",
		    (unsigned long)sizeof(cfg.work_folder));
		return 2;
	}

	f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
		fprintf(stderr, "%s: cannot determine the size\n", path);
		goto out;
	}
	rewind(f);

	message = malloc((size_t)len + 1);
	if (message == NULL) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	if (fread(message, 1, (size_t)len, f) != (size_t)len) {
		fprintf(stderr, "%s: short read of %lu bytes\n", path,
		    (unsigned long)len);
		goto out;
	}
	message[len] = '\0';

	http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	if (http == NULL) {
		fprintf(stderr, "http client: %s\n", sgug_http_last_error());
		goto out;
	}

	signal(SIGPIPE, SIG_IGN);

	if (cancel_path != NULL) {
		struct sigaction sa;
		struct stat st;

		if (stat(cancel_path, &st) != 0) {
			fprintf(stderr, "%s: %s\n", cancel_path,
			    strerror(errno));
			goto out;
		}
		cancel_mtime = (sgug_time_t)st.st_mtime;

		/*
		 * SA_RESTART, unlike the shutdown signals below: a bare poll
		 * in sgug_tcp_connect and the reads inside SSL_connect treat
		 * EINTR as fatal, so a repeating alarm would fail connections
		 * at random. Nothing needs the interruption; step.c reaches
		 * the flag through its own 500ms poll timeout.
		 */
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_cancel_alarm;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(SIGALRM, &sa, NULL);
		alarm(CANCEL_POLL_SECS);
	}

	/* Cancel rather than die, so the job still reports a result. Without
	 * this a docker stop leaves the service holding the slot forever. */
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_cancel_signal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	memset(&cfg, 0, sizeof(cfg));
	sgug_snprintf(cfg.agent_name, sizeof(cfg.agent_name), "%s", name);
	sgug_snprintf(cfg.work_folder, sizeof(cfg.work_folder), "%s", work);

	err[0] = '\0';
	if (sgug_run_job(http, &cfg, message, (size_t)len, &cancel_requested,
	    err, sizeof(err)) != 0) {
		fprintf(stderr, "execjob: %s\n", err);
		goto out;
	}
	rc = 0;

out:
	if (f != NULL)
		fclose(f);
	free(message);
	sgug_http_client_free(http);
	return rc;
}

static int
cmd_status(int argc, char **argv)
{
	sgug_config cfg;
	const char *dir = ".";
	int i;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--dir") == 0)
			dir = arg_after(argc, argv, &i, "--dir");
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	if (sgug_config_load(&cfg, dir) != 0) {
		fprintf(stderr, "no runner configured in %s\n", dir);
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
 *
 * A Linux host gets both of those right from NTP and the distro bundle, so the
 * docker socket is the one that actually decides whether `serve` works.
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

	if (sgug_serve_selftest() != 0)
		goto out;

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
	if (strcmp(argv[1], "serve") == 0)
		return cmd_serve(argc, argv);
	if (strcmp(argv[1], "execjob") == 0)
		return cmd_execjob(argc, argv);
	if (strcmp(argv[1], "status") == 0)
		return cmd_status(argc, argv);

	fprintf(stderr, "unknown command: %s\n\n", argv[1]);
	return usage(stderr, 2);
}
