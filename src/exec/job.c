#include "exec/job.h"

#include "crypto/b64.h"
#include "exec/token.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* SystemVssConnection carries the job's own credentials and service addresses. */
static int
parse_endpoint(sgug_job *job, const sgug_json *root, char *err, size_t errlen)
{
	const sgug_json *eps = sgug_json_path(root, "resources.endpoints");
	size_t i, n = sgug_json_len(eps);

	for (i = 0; i < n; i++) {
		const sgug_json *e = sgug_json_at(eps, i);
		const char *name = sgug_json_string(sgug_json_get(e, "name"), "");

		if (strcmp(name, "SystemVssConnection") != 0)
			continue;

		job->service_url = sgug_json_string(sgug_json_get(e, "url"), "");
		job->access_token = sgug_json_string(
		    sgug_json_path(e, "authorization.parameters.AccessToken"), "");
		job->pipelines_url = sgug_json_string(
		    sgug_json_path(e, "data.PipelinesServiceUrl"), "");
		job->results_url = sgug_json_string(
		    sgug_json_path(e, "data.ResultsServiceUrl"), "");

		if (job->access_token[0] == '\0') {
			seterr(err, errlen,
			    "SystemVssConnection carried no AccessToken");
			return -1;
		}
		return 0;
	}

	seterr(err, errlen, "job message had no SystemVssConnection endpoint");
	return -1;
}

static void
parse_step(sgug_step *st, const sgug_json *s)
{
	const sgug_json *ref, *inputs, *v;
	const char *reftype;

	memset(st, 0, sizeof(*st));

	st->id = sgug_json_string(sgug_json_get(s, "id"), "");
	st->context_name = sgug_json_string(sgug_json_get(s, "contextName"), "");
	st->condition = sgug_json_string(sgug_json_get(s, "condition"),
	    "success()");
	st->display_name = sgug_token_str(sgug_json_get(s, "displayNameToken"),
	    NULL);

	ref = sgug_json_get(s, "reference");
	reftype = sgug_json_string(sgug_json_get(ref, "type"), "");

	inputs = sgug_json_get(s, "inputs");

	if (strcmp(reftype, "script") == 0) {
		st->kind = SGUG_STEP_SCRIPT;
		/*
		 * The shell body lives under the "script" key of the inputs
		 * mapping, in the verbose TemplateToken form. Read with the
		 * compact spelling it comes back empty and the step runs
		 * nothing at all, silently.
		 */
		v = sgug_token_map_get(inputs, "script");
		if (v != NULL && !sgug_token_is_literal(v)) {
			/* Without this the script reads as empty and the step
			 * is reported as having succeeded without running. */
			st->kind = SGUG_STEP_UNSUPPORTED;
			st->unsupported_reason =
			    "this step's script is a ${{ }} expression, and "
			    "this runner has no expression evaluator yet. Use "
			    "the $GITHUB_* shell variables instead, or run the "
			    "step on a hosted runner. See docs/protocol.md.";
		} else {
			st->script = sgug_token_str(v, "");
			v = sgug_token_map_get(inputs, "shell");
			if (v != NULL)
				st->shell = sgug_token_str(v, NULL);
			v = sgug_token_map_get(inputs, "workingDirectory");
			if (v != NULL)
				st->working_directory = sgug_token_str(v, NULL);
		}
	} else if (strcmp(reftype, "repository") == 0) {
		st->kind = SGUG_STEP_ACTION;
		st->action_name = sgug_json_string(sgug_json_get(ref, "name"), "");
		st->action_ref = sgug_json_string(sgug_json_get(ref, "ref"), "");
		st->inputs = inputs;
	} else {
		st->kind = SGUG_STEP_UNSUPPORTED;
		st->action_name = reftype;
		st->unsupported_reason =
		    "container and docker steps cannot run on IRIX: there is "
		    "no container runtime";
	}

	/*
	 * A `uses:` step carries no displayNameToken, so without this it shows
	 * as its generated context name, __actions_checkout. The official
	 * runner labels it "Run owner/action@ref".
	 */
	if (st->display_name == NULL || *st->display_name == '\0') {
		if (st->kind == SGUG_STEP_ACTION && st->action_name != NULL &&
		    *st->action_name != '\0') {
			if (st->action_ref != NULL && *st->action_ref != '\0')
				sgug_snprintf(st->label_buf,
				    sizeof(st->label_buf), "Run %s@%s",
				    st->action_name, st->action_ref);
			else
				sgug_snprintf(st->label_buf,
				    sizeof(st->label_buf), "Run %s",
				    st->action_name);
			st->display_name = st->label_buf;
		} else {
			st->display_name = st->context_name;
		}
	}

	v = sgug_json_get(s, "continueOnError");
	st->continue_on_error =
	    sgug_json_bool(v, 0) || strcmp(sgug_token_str(v, ""), "true") == 0;

	v = sgug_json_get(s, "timeoutInMinutes");
	st->timeout_minutes = (int)sgug_json_int(v, 0);
}

/*
 * Pulls the results backend ids out of the job token.
 *
 * The token is a JWT; its payload carries an scp claim listing scopes
 * separated by spaces, one of which is
 * "Actions.Results:<runBackendId>:<jobRunBackendId>".
 */
static void
parse_backend_ids(sgug_job *job)
{
	const char *tok = job->access_token;
	const char *dot1, *dot2;
	char seg[4096];
	unsigned char payload[4096];
	sgug_json_doc *doc;
	const char *scp, *p;
	size_t n;
	int plen;

	if (tok == NULL || *tok == '\0')
		return;

	dot1 = strchr(tok, '.');
	if (dot1 == NULL)
		return;
	dot2 = strchr(dot1 + 1, '.');
	if (dot2 == NULL)
		return;

	n = (size_t)(dot2 - dot1 - 1);
	if (n == 0 || n >= sizeof(seg))
		return;
	memcpy(seg, dot1 + 1, n);
	seg[n] = '\0';

	plen = sgug_b64_decode(seg, payload, sizeof(payload) - 1);
	if (plen <= 0)
		return;
	payload[plen] = '\0';

	doc = sgug_json_parse((const char *)payload, (size_t)plen, NULL, 0);
	if (doc == NULL)
		return;

	scp = sgug_json_string(sgug_json_get(sgug_json_root(doc), "scp"), NULL);
	for (p = scp; p != NULL && *p != '\0'; ) {
		static const char PREFIX[] = "Actions.Results:";
		const char *end = strchr(p, ' ');
		size_t seglen = end != NULL ? (size_t)(end - p) : strlen(p);

		if (seglen > sizeof(PREFIX) - 1 &&
		    strncmp(p, PREFIX, sizeof(PREFIX) - 1) == 0) {
			const char *ids = p + sizeof(PREFIX) - 1;
			const char *colon = memchr(ids, ':',
			    seglen - (sizeof(PREFIX) - 1));

			if (colon != NULL) {
				size_t a = (size_t)(colon - ids);
				size_t b = seglen - (sizeof(PREFIX) - 1) - a - 1;

				if (a < sizeof(job->backend_run_id) &&
				    b < sizeof(job->backend_job_id)) {
					memcpy(job->backend_run_id, ids, a);
					job->backend_run_id[a] = '\0';
					memcpy(job->backend_job_id, colon + 1, b);
					job->backend_job_id[b] = '\0';
				}
			}
			break;
		}

		if (end == NULL)
			break;
		p = end + 1;
	}

	sgug_json_free(doc);
}

int
sgug_job_parse(const char *text, size_t len, sgug_job *out, char *err,
    size_t errlen)
{
	const sgug_json *root, *steps, *mask;
	char perr[128];
	size_t i, n;

	memset(out, 0, sizeof(*out));

	perr[0] = '\0';
	out->doc = sgug_json_parse(text, len, perr, sizeof(perr));
	if (out->doc == NULL) {
		seterr(err, errlen, "job message was not JSON: %s", perr);
		return -1;
	}
	root = sgug_json_root(out->doc);

	out->plan_id = sgug_json_string(sgug_json_path(root, "plan.planId"), "");
	/* "actions" here, not "Build" as the reference says. It is the hub name
	 * in every timeline and log URL, so getting it wrong 404s all
	 * reporting. */
	out->plan_type = sgug_json_string(sgug_json_path(root, "plan.planType"),
	    "actions");
	out->timeline_id = sgug_json_string(sgug_json_path(root, "timeline.id"),
	    out->plan_id);
	out->job_id = sgug_json_string(sgug_json_get(root, "jobId"), "");
	out->job_name = sgug_json_string(sgug_json_get(root, "jobName"), "");
	out->job_display_name = sgug_json_string(
	    sgug_json_get(root, "jobDisplayName"), out->job_name);
	out->request_id = sgug_json_int(sgug_json_get(root, "requestId"), 0);
	out->billing_owner_id = sgug_json_string(
	    sgug_json_get(root, "billingOwnerId"), "");

	if (out->plan_id[0] == '\0' || out->job_id[0] == '\0') {
		seterr(err, errlen, "job message lacked planId or jobId");
		goto fail;
	}

	if (parse_endpoint(out, root, err, errlen) != 0)
		goto fail;

	parse_backend_ids(out);
	/* Fall back to the plan and job ids, which is right for a single job
	 * and is what older deployments used. */
	if (out->backend_run_id[0] == '\0')
		sgug_snprintf(out->backend_run_id, sizeof(out->backend_run_id),
		    "%s", out->plan_id);
	if (out->backend_job_id[0] == '\0')
		sgug_snprintf(out->backend_job_id, sizeof(out->backend_job_id),
		    "%s", out->job_id);

	steps = sgug_json_get(root, "steps");
	n = sgug_json_len(steps);
	if (n > SGUG_JOB_MAX_STEPS)
		n = SGUG_JOB_MAX_STEPS;
	for (i = 0; i < n; i++)
		parse_step(&out->steps[i], sgug_json_at(steps, i));
	out->nsteps = n;

	/*
	 * Literal secrets only. Entries of type "regex" need a regex engine to
	 * be useful and the values that actually matter, the installation
	 * token and its fragments, arrive as literals.
	 */
	mask = sgug_json_get(root, "mask");
	n = sgug_json_len(mask);
	for (i = 0; i < n && out->nmasks < 64; i++) {
		const sgug_json *m = sgug_json_at(mask, i);
		const char *type = sgug_json_string(sgug_json_get(m, "type"), "");
		const char *val = sgug_json_string(sgug_json_get(m, "value"), "");

		if (strcmp(type, "regex") == 0 || val[0] == '\0')
			continue;
		out->masks[out->nmasks++] = val;
	}

	return 0;

fail:
	sgug_json_free(out->doc);
	out->doc = NULL;
	return -1;
}

void
sgug_job_free(sgug_job *job)
{
	if (job == NULL)
		return;
	sgug_json_free(job->doc);
	memset(job, 0, sizeof(*job));
}

const char *
sgug_job_variable(const sgug_job *job, const char *name, const char *fallback)
{
	const sgug_json *v;

	if (job == NULL || job->doc == NULL)
		return fallback;

	v = sgug_json_get(
	    sgug_json_get(sgug_json_root(job->doc), "variables"), name);

	return sgug_json_string(sgug_json_get(v, "value"), fallback);
}

const char *
sgug_job_context(const sgug_job *job, const char *context, const char *key,
    const char *fallback)
{
	const sgug_json *ctx;

	if (job == NULL || job->doc == NULL)
		return fallback;

	ctx = sgug_json_get(
	    sgug_json_get(sgug_json_root(job->doc), "contextData"), context);

	return sgug_token_map_str(ctx, key, fallback);
}
