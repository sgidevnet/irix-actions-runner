#include "exec/handlers.h"

#include "crypto/b64.h"
#include "exec/token.h"
#include "net/http.h"
#include "proto/results.h"

#include "version.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
say(sgug_step_output_fn on_line, void *ctx, const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	sgug_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (on_line != NULL)
		on_line(ctx, line);
}

/*
 * git, with the ambient environment neutralised.
 *
 * A developer's ~/.gitconfig is not the runner's business, and on this machine
 * it sets http.sslverify=false, which a job must not inherit.
 */
static int
git(const sgug_step_opts *opts, const char *cwd, sgug_step_output_fn on_line,
    void *ctx, char *err, size_t errlen, ...)
{
	const char *argv[24];
	va_list ap;
	size_t n = 0;

	argv[n++] = "git";
	argv[n++] = "-c";
	argv[n++] = "http.sslVerify=true";

	va_start(ap, errlen);
	while (n < sizeof(argv) / sizeof(argv[0]) - 1) {
		const char *a = va_arg(ap, const char *);

		if (a == NULL)
			break;
		argv[n++] = a;
	}
	va_end(ap);
	argv[n] = NULL;

	return sgug_run_argv(argv, cwd, opts, on_line, ctx, err, errlen);
}

/*
 * actions/checkout.
 *
 * The real action is about 1200 lines of TypeScript that mostly assembles git
 * command lines, so this reimplements the path an ordinary workflow takes:
 * init, point at the remote, authenticate, fetch one commit, check it out.
 *
 * Not implemented: submodules, LFS, sparse checkout, and the tarball fallback
 * for when git is missing or too old. The Octane has git 2.22, comfortably
 * above the 2.18 the real action requires, so the fallback is moot.
 */
static int
action_checkout(const sgug_job *job, const sgug_step *step,
    const sgug_step_opts *opts, sgug_step_output_fn on_line, void *ctx,
    char *err, size_t errlen)
{
	char url[512];
	char header[1024];
	/*
	 * Sized for the installation token, which is a ghs_ value of roughly
	 * 380 characters. With the "x-access-token:" prefix that base64 encodes
	 * to about 525 bytes, so a 512 byte buffer fails by a margin small
	 * enough to look like something else.
	 */
	char authval[1024];
	char basic[1536];
	char depthbuf[32];
	const char *repo, *ref, *sha, *token, *server;
	const char *depth, *path, *persist;
	char target[512];
	int rc;

	repo = sgug_token_map_str(step->inputs, "repository",
	    sgug_job_context(job, "github", "repository", NULL));
	if (repo == NULL || *repo == '\0') {
		seterr(err, errlen, "checkout: no repository to check out");
		return -1;
	}

	server = sgug_job_context(job, "github", "server_url", "https://github.com");
	sha = sgug_job_context(job, "github", "sha", NULL);
	ref = sgug_token_map_str(step->inputs, "ref",
	    sgug_job_context(job, "github", "ref", NULL));

	/*
	 * The installation token, which is a job variable rather than part of
	 * any context. Without it only public repositories can be fetched.
	 */
	token = sgug_token_map_str(step->inputs, "token",
	    sgug_job_variable(job, "github_token",
	    sgug_job_variable(job, "system.github.token", NULL)));

	depth = sgug_token_map_str(step->inputs, "fetch-depth", "1");
	path = sgug_token_map_str(step->inputs, "path", NULL);
	persist = sgug_token_map_str(step->inputs, "persist-credentials", "true");

	if (path != NULL && *path != '\0')
		sgug_snprintf(target, sizeof(target), "%s/%s", opts->work_dir, path);
	else
		sgug_snprintf(target, sizeof(target), "%s", opts->work_dir);

	if (mkdir(target, 0755) != 0 && errno != EEXIST) {
		seterr(err, errlen, "checkout: cannot create %s", target);
		return -1;
	}

	say(on_line, ctx, "Checking out %s into %s", repo, target);

	sgug_snprintf(url, sizeof(url), "%s/%s", server, repo);

	rc = git(opts, target, on_line, ctx, err, errlen, "init", "--quiet", NULL);
	if (rc != 0)
		goto fail;

	/* set-url after add so a re-run on a dirty workspace is not an error. */
	git(opts, target, NULL, ctx, NULL, 0, "remote", "add", "origin", url, NULL);
	rc = git(opts, target, on_line, ctx, err, errlen, "remote", "set-url",
	    "origin", url, NULL);
	if (rc != 0)
		goto fail;

	if (token != NULL && *token != '\0') {
		/*
		 * The token goes in a git config value, never on a command line
		 * and never in the remote URL, so it cannot leak through ps or
		 * through .git/config's remote entry. sgug_run_argv passes it
		 * as one argv element, so no quoting is involved.
		 */
		sgug_snprintf(authval, sizeof(authval), "x-access-token:%s", token);
		if (sgug_b64_encode(authval, strlen(authval), basic,
		    sizeof(basic)) < 0) {
			seterr(err, errlen, "checkout: token too long to encode");
			goto fail;
		}
		sgug_snprintf(header, sizeof(header),
		    "http.%s/.extraheader", server);

		{
			char hv[1700];

			sgug_snprintf(hv, sizeof(hv), "AUTHORIZATION: basic %s",
			    basic);
			rc = git(opts, target, NULL, ctx, err, errlen, "config",
			    "--local", header, hv, NULL);
			explicit_bzero(hv, sizeof(hv));
		}
		explicit_bzero(authval, sizeof(authval));
		explicit_bzero(basic, sizeof(basic));

		if (rc != 0) {
			seterr(err, errlen, "checkout: cannot set the auth header");
			goto fail;
		}
	}

	sgug_snprintf(depthbuf, sizeof(depthbuf), "--depth=%s",
	    depth != NULL && *depth != '\0' ? depth : "1");

	/*
	 * Fetch the commit rather than the branch. A branch can move between
	 * the job being queued and the checkout running, and the workflow is
	 * meant to build the commit it was triggered for.
	 */
	if (sha != NULL && *sha != '\0')
		rc = git(opts, target, on_line, ctx, err, errlen, "fetch",
		    strcmp(depthbuf, "--depth=0") == 0 ? "--tags" : depthbuf,
		    "origin", sha, NULL);
	else if (ref != NULL && *ref != '\0')
		rc = git(opts, target, on_line, ctx, err, errlen, "fetch",
		    depthbuf, "origin", ref, NULL);
	else {
		seterr(err, errlen, "checkout: neither a sha nor a ref to fetch");
		goto fail;
	}
	if (rc != 0)
		goto fail;

	rc = git(opts, target, on_line, ctx, err, errlen, "checkout", "--force",
	    "FETCH_HEAD", NULL);
	if (rc != 0)
		goto fail;

	/*
	 * The header is removed unless the workflow asked to keep it. Leaving a
	 * credential in .git/config of a workspace that outlives the job is how
	 * a later job on the same runner inherits access it was never granted.
	 */
	if (token != NULL && *token != '\0' && strcmp(persist, "true") != 0) {
		sgug_snprintf(header, sizeof(header), "http.%s/.extraheader",
		    server);
		git(opts, target, NULL, ctx, NULL, 0, "config", "--local",
		    "--unset-all", header, NULL);
	}

	say(on_line, ctx, "Checked out %s", sha != NULL ? sha : ref);
	return 0;

fail:
	return rc != 0 ? rc : -1;
}

/*
 * A results client for the artifact handlers.
 *
 * Built per call rather than shared: a handler receives the job but not the
 * reporter that owns the long-lived one, and an artifact operation is rare
 * enough that the extra setup is irrelevant next to the transfer.
 */
static sgug_results *
results_for(const sgug_job *job, sgug_http_client **http_out)
{
	sgug_http_client *http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	sgug_results *r;

	if (http == NULL)
		return NULL;

	r = sgug_results_new(http, job);
	if (r == NULL) {
		sgug_http_client_free(http);
		return NULL;
	}
	*http_out = http;
	return r;
}

/*
 * actions/upload-artifact.
 *
 * Artifacts v4 are zip archives, so this builds one with the zip binary the
 * same way checkout drives git, then does the three call upload: create, PUT
 * to the signed URL, finalize with the size and a sha256.
 *
 * Not implemented: multiple path patterns, exclusions, and the compression
 * level input. A single path or glob covers what a build produces.
 */
static int
action_upload_artifact(const sgug_job *job, const sgug_step *step,
    const sgug_step_opts *opts, sgug_step_output_fn on_line, void *ctx,
    char *err, size_t errlen)
{
	sgug_http_client *http = NULL;
	sgug_results *r = NULL;
	const char *argv[8];
	char zippath[512];
	const char *name, *path;
	int rc;

	name = sgug_token_map_str(step->inputs, "name", "artifact");
	path = sgug_token_map_str(step->inputs, "path", NULL);

	if (path == NULL || *path == '\0') {
		seterr(err, errlen, "upload-artifact: no path given");
		return -1;
	}

	sgug_snprintf(zippath, sizeof(zippath), "%s/artifact-%ld.zip",
	    opts->temp_dir, (long)getpid());

	say(on_line, ctx, "Zipping %s", path);

	argv[0] = "zip";
	argv[1] = "-q";
	argv[2] = "-r";
	argv[3] = zippath;
	argv[4] = path;
	argv[5] = NULL;

	rc = sgug_run_argv(argv, opts->work_dir, opts, on_line, ctx, err, errlen);
	if (rc != 0) {
		seterr(err, errlen, "upload-artifact: zip failed (%d)", rc);
		return rc != 0 ? rc : -1;
	}

	r = results_for(job, &http);
	if (r == NULL) {
		seterr(err, errlen, "upload-artifact: no results service for "
		    "this job");
		unlink(zippath);
		return -1;
	}

	say(on_line, ctx, "Uploading artifact %s", name);
	rc = sgug_artifact_upload(r, name, zippath, err, errlen);
	if (rc == 0)
		say(on_line, ctx, "Uploaded %s", name);

	unlink(zippath);
	sgug_results_free(r);
	sgug_http_client_free(http);
	return rc;
}

/* actions/download-artifact. Fetches the zip and unpacks it. */
static int
action_download_artifact(const sgug_job *job, const sgug_step *step,
    const sgug_step_opts *opts, sgug_step_output_fn on_line, void *ctx,
    char *err, size_t errlen)
{
	sgug_http_client *http = NULL;
	sgug_results *r = NULL;
	const char *argv[8];
	char zippath[512];
	char dest[512];
	const char *name, *path;
	int rc;

	name = sgug_token_map_str(step->inputs, "name", "artifact");
	path = sgug_token_map_str(step->inputs, "path", NULL);

	if (path != NULL && *path != '\0')
		sgug_snprintf(dest, sizeof(dest), "%s/%s", opts->work_dir, path);
	else
		sgug_snprintf(dest, sizeof(dest), "%s", opts->work_dir);

	if (mkdir(dest, 0755) != 0 && errno != EEXIST) {
		seterr(err, errlen, "download-artifact: cannot create %s", dest);
		return -1;
	}

	sgug_snprintf(zippath, sizeof(zippath), "%s/download-%ld.zip",
	    opts->temp_dir, (long)getpid());

	r = results_for(job, &http);
	if (r == NULL) {
		seterr(err, errlen, "download-artifact: no results service for "
		    "this job");
		return -1;
	}

	say(on_line, ctx, "Downloading artifact %s", name);
	rc = sgug_artifact_download(r, name, zippath, err, errlen);

	sgug_results_free(r);
	sgug_http_client_free(http);

	if (rc != 0)
		return rc;

	argv[0] = "unzip";
	argv[1] = "-q";
	argv[2] = "-o";
	argv[3] = zippath;
	argv[4] = "-d";
	argv[5] = dest;
	argv[6] = NULL;

	rc = sgug_run_argv(argv, opts->work_dir, opts, on_line, ctx, err, errlen);
	unlink(zippath);

	if (rc != 0) {
		seterr(err, errlen, "download-artifact: unzip failed (%d)", rc);
		return rc;
	}

	say(on_line, ctx, "Extracted %s into %s", name, dest);
	return 0;
}

static const struct {
	const char *name;
	sgug_action_fn fn;
} HANDLERS[] = {
	{ "actions/checkout", action_checkout },
	{ "actions/upload-artifact", action_upload_artifact },
	{ "actions/download-artifact", action_download_artifact }
};

sgug_action_fn
sgug_action_lookup(const char *action_name)
{
	size_t i;

	if (action_name == NULL)
		return NULL;

	for (i = 0; i < sizeof(HANDLERS) / sizeof(HANDLERS[0]); i++) {
		if (strcmp(HANDLERS[i].name, action_name) == 0)
			return HANDLERS[i].fn;
	}
	return NULL;
}

const char *
sgug_action_supported(void)
{
	return "actions/checkout, actions/upload-artifact, "
	    "actions/download-artifact";
}
