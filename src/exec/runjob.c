#include "exec/runjob.h"

#include "exec/handlers.h"
#include "exec/job.h"
#include "exec/step.h"
#include "proto/report.h"
#include "version.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_ENV 64

struct runctx {
	sgug_reporter *rep;
	size_t step;
	volatile sig_atomic_t *stop;
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

static void
on_line(void *ctx, const char *line)
{
	struct runctx *rc = ctx;

	sgug_report_step_output(rc->rep, rc->step, line);
	/* Also to our own stdout, so an operator watching the runner sees the
	 * same thing the web UI does. */
	printf("  | %s\n", line);
	fflush(stdout);
}

static int
should_abort(void *ctx)
{
	struct runctx *rc = ctx;

	return rc->stop != NULL && *rc->stop;
}

/* mkdir -p. IRIX has no *at family, so this is the plain recursive form. */
static int
mkpath(const char *path)
{
	char tmp[512];
	char *p;

	sgug_snprintf(tmp, sizeof(tmp), "%s", path);

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static char *
envdup(const char *name, const char *value)
{
	size_t n = strlen(name) + strlen(value) + 2;
	char *s = malloc(n);

	if (s != NULL)
		sgug_snprintf(s, n, "%s=%s", name, value);
	return s;
}

/*
 * Builds the step environment from scratch.
 *
 * Not inherited: the runner's own environment holds its credentials, and the
 * invoking user's profile can carry settings a job must not pick up, such as a
 * global git http.sslverify=false.
 */
static size_t
build_env(const sgug_job *job, const sgug_config *cfg, const char *workspace,
    const char *temp, char **env, size_t max)
{
	static const char *const PASS_GITHUB[] = {
		"repository", "repository_owner", "ref", "ref_name", "sha",
		"run_id", "run_number", "run_attempt", "actor", "workflow",
		"event_name", "job", "server_url", "api_url", "graphql_url",
		"triggering_actor", "base_ref", "head_ref", "ref_type"
	};
	size_t n = 0, i;
	char name[64];

	env[n++] = envdup("PATH",
	    "/usr/sgug/bin:/usr/sgug/sbin:/usr/bin:/bin:/usr/sbin:/usr/bsd");
	env[n++] = envdup("HOME", getenv("HOME") != NULL ? getenv("HOME") : "/");
	env[n++] = envdup("SHELL", "/usr/sgug/bin/bash");
	env[n++] = envdup("LC_ALL", "C");
	env[n++] = envdup("TERM", "dumb");

	/*
	 * n32 binaries resolve libraries through LD_LIBRARYN32_PATH, not
	 * LD_LIBRARY_PATH. Without it anything built against SGUG fails to
	 * start with a bare rld error.
	 */
	env[n++] = envdup("LD_LIBRARYN32_PATH",
	    "/usr/sgug/lib32:/usr/lib32:/lib32:/usr/lib:/lib");

	/* Claimed as Linux so ordinary workflow conditionals behave. The truth
	 * is in the runner labels. */
	env[n++] = envdup("RUNNER_OS", sgug_runner_os());
	env[n++] = envdup("RUNNER_ARCH", "X64");
	env[n++] = envdup("RUNNER_NAME", cfg->agent_name);
	env[n++] = envdup("RUNNER_TEMP", temp);
	env[n++] = envdup("RUNNER_WORKSPACE", workspace);
	env[n++] = envdup("RUNNER_TOOL_CACHE", temp);

	env[n++] = envdup("CI", "true");
	env[n++] = envdup("GITHUB_ACTIONS", "true");
	env[n++] = envdup("GITHUB_WORKSPACE", workspace);
	env[n++] = envdup("GITHUB_JOB", job->job_name);

	/* Ambient git configuration must not leak into a job. */
	env[n++] = envdup("GIT_CONFIG_NOSYSTEM", "1");

	for (i = 0; i < sizeof(PASS_GITHUB) / sizeof(PASS_GITHUB[0]) &&
	    n < max - 2; i++) {
		const char *v = sgug_job_context(job, "github", PASS_GITHUB[i],
		    NULL);
		size_t j;

		if (v == NULL)
			continue;

		sgug_snprintf(name, sizeof(name), "GITHUB_%s", PASS_GITHUB[i]);
		for (j = 7; name[j] != '\0'; j++) {
			if (name[j] >= 'a' && name[j] <= 'z')
				name[j] = (char)(name[j] - 'a' + 'A');
		}
		env[n++] = envdup(name, v);
	}

	env[n] = NULL;
	return n;
}

int
sgug_run_job(sgug_http_client *http, const sgug_config *cfg,
    const char *message, size_t message_len,
    volatile sig_atomic_t *stop, char *err, size_t errlen)
{
	sgug_job job;
	sgug_reporter *rep = NULL;
	sgug_step_opts sopts;
	struct runctx rc;
	char *env[MAX_ENV];
	char workspace[512], temp[512], root[512];
	const char *repo;
	const char *slash;
	size_t i, nenv = 0;
	int failed = 0, cancelled = 0;
	int result = 0;

	if (sgug_job_parse(message, message_len, &job, err, errlen) != 0)
		return -1;

	/*
	 * _work/<repo>/<repo> mirrors the official runner's layout, which some
	 * workflows and actions assume when they compute paths.
	 */
	/*
	 * Absolute, because a step chdirs into its working directory before
	 * exec and any relative path we handed it stops resolving. The script
	 * file failed with "No such file or directory" this way.
	 */
	if (cfg->work_folder[0] == '/') {
		sgug_snprintf(root, sizeof(root), "%s", cfg->work_folder);
	} else if (getcwd(root, sizeof(root)) != NULL) {
		size_t used = strlen(root);

		sgug_snprintf(root + used, sizeof(root) - used, "/%s",
		    cfg->work_folder);
	} else {
		seterr(err, errlen, "cannot resolve the work folder");
		sgug_job_free(&job);
		return -1;
	}

	repo = sgug_job_context(&job, "github", "repository", "work");
	slash = strrchr(repo, '/');
	sgug_snprintf(workspace, sizeof(workspace), "%s/%s/%s",
	    root, slash != NULL ? slash + 1 : repo,
	    slash != NULL ? slash + 1 : repo);
	sgug_snprintf(temp, sizeof(temp), "%s/_temp", root);

	if (mkpath(workspace) != 0 || mkpath(temp) != 0) {
		seterr(err, errlen, "cannot create %s", workspace);
		sgug_job_free(&job);
		return -1;
	}

	rep = sgug_report_new(http, &job);
	if (rep == NULL) {
		sgug_job_free(&job);
		return -1;
	}

	printf("job       %s (%lu steps)\n", job.job_display_name,
	    (unsigned long)job.nsteps);
	if (job.nsteps == 0 && getenv("SGUG_DUMP_EMPTY") != NULL) {
		FILE *f = fopen("/tmp/empty-job.json", "wb");

		if (f != NULL) {
			fwrite(message, 1, message_len, f);
			fclose(f);
			printf("          zero steps, message saved\n");
		}
	}
	fflush(stdout);

	sgug_report_begin(rep);
	sgug_report_job_started(rep);

	nenv = build_env(&job, cfg, workspace, temp, env, MAX_ENV);

	memset(&sopts, 0, sizeof(sopts));
	sopts.work_dir = workspace;
	sopts.temp_dir = temp;
	sopts.env = env;
	sopts.nenv = nenv;
	sopts.timeout_seconds = 3600;

	rc.rep = rep;
	rc.stop = stop;

	for (i = 0; i < job.nsteps; i++) {
		const sgug_step *st = &job.steps[i];
		char steperr[256];
		int status;

		if (stop != NULL && *stop)
			cancelled = 1;

		if (!sgug_step_should_run(st->condition, failed, cancelled)) {
			sgug_report_step_finished(rep, i, SGUG_RESULT_SKIPPED);
			printf("step      %s (skipped)\n", st->display_name);
			fflush(stdout);
			continue;
		}

		printf("step      %s\n", st->display_name);
		fflush(stdout);

		sgug_report_step_started(rep, i);
		rc.step = i;

		sopts.abort_cb = should_abort;
		sopts.abort_ctx = &rc;
		steperr[0] = '\0';

		if (st->kind == SGUG_STEP_ACTION) {
			sgug_action_fn h = sgug_action_lookup(st->action_name);

			if (h == NULL) {
				char msg[320];

				sgug_snprintf(msg, sizeof(msg),
				    "%s has no native handler. This runner has "
				    "no JavaScript engine, so only these are "
				    "supported: %s",
				    st->action_name, sgug_action_supported());
				on_line(&rc, msg);
				sgug_report_step_finished(rep, i,
				    SGUG_RESULT_FAILED);
				failed = 1;
				continue;
			}
			status = h(&job, st, &sopts, on_line, &rc, steperr,
			    sizeof(steperr));
		} else if (st->kind != SGUG_STEP_SCRIPT) {
			char msg[256];

			sgug_snprintf(msg, sizeof(msg),
			    "container and docker steps cannot run on IRIX: "
			    "there is no container runtime");
			on_line(&rc, msg);
			sgug_report_step_finished(rep, i, SGUG_RESULT_FAILED);
			failed = 1;
			continue;
		} else {
			status = sgug_step_run(st, &sopts, on_line, &rc, steperr,
			    sizeof(steperr));
		}

		sgug_report_flush(rep);

		if (status == SGUG_STEP_ABORTED) {
			cancelled = 1;
			sgug_report_step_finished(rep, i, SGUG_RESULT_CANCELED);
		} else if (status < 0) {
			on_line(&rc, steperr);
			failed = 1;
			sgug_report_step_finished(rep, i, SGUG_RESULT_FAILED);
		} else if (status != 0) {
			char msg[128];

			sgug_snprintf(msg, sizeof(msg),
			    "Process completed with exit code %d.", status);
			on_line(&rc, msg);
			if (!st->continue_on_error)
				failed = 1;
			sgug_report_step_finished(rep, i,
			    st->continue_on_error ? SGUG_RESULT_SUCCEEDED
			    : SGUG_RESULT_FAILED);
		} else {
			sgug_report_step_finished(rep, i, SGUG_RESULT_SUCCEEDED);
		}
	}

	result = cancelled ? SGUG_RESULT_CANCELED
	    : failed ? SGUG_RESULT_FAILED : SGUG_RESULT_SUCCEEDED;

	if (sgug_report_job_finished(rep, (sgug_result)result) != 0) {
		seterr(err, errlen, "could not report completion: %s",
		    sgug_report_last_error(rep));
		result = -1;
	}

	printf("job       %s\n", cancelled ? "cancelled" :
	    failed ? "failed" : "succeeded");
	fflush(stdout);

	for (i = 0; i < nenv; i++)
		free(env[i]);
	sgug_report_free(rep);
	sgug_job_free(&job);

	return result == -1 ? -1 : 0;
}
