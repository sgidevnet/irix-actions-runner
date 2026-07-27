#include "proto/report.h"

#include "crypto/b64.h"
#include "json/json.h"
#include "proto/config.h"
#include "proto/results.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API "5.1-preview.1"
#define API_EVENTS "2.0-preview.1"
#define TIMEOUT_MS 30000

/* The live feed is best effort; the uploaded log is the record that persists.
 * Dropping lines beyond this keeps a runaway step from exhausting memory on a
 * machine with 2 GB. */
#define MAX_FEED_LINES 1024
#define MAX_LINE 1024

struct step_state {
	char record_id[40];
	int64_t log_id;
	char started[40];
	char finished[40];
	sgug_result result;
	int ran;
	int64_t log_lines;
	/* Whole-step output, uploaded once at step end. */
	char *log;
	size_t log_len;
	size_t log_cap;
	/* Pending console lines, flushed periodically. */
	char *feed[MAX_FEED_LINES];
	size_t nfeed;
	int64_t lines_sent;
};

struct sgug_reporter {
	sgug_http_client *http;
	const sgug_job *job;

	char auth[4096];
	char base[SGUG_MAX_URL];	/* hubs/{type}/plans/{planId} */
	char job_started[40];

	struct step_state steps[SGUG_JOB_MAX_STEPS];

	/*
	 * True when this deployment still serves the Azure DevOps timeline and
	 * log APIs. Modern github.com does not: the official runner v2.336.0
	 * contains no _apis/distributedtask routes at all, only run-service
	 * verbs and a Twirp results service, and every timeline call returns
	 * 404. Detected by the presence of ResultsServiceUrl in the job.
	 */
	int legacy_timeline;

	/* NULL on deployments that still use the timeline API. */
	sgug_results *results;
	int steps_update_broken;

	char err[512];
};

static const char *
result_name(sgug_result r)
{
	switch (r) {
	case SGUG_RESULT_SUCCEEDED: return "succeeded";
	case SGUG_RESULT_FAILED: return "failed";
	case SGUG_RESULT_CANCELED: return "canceled";
	default: return "skipped";
	}
}

const char *
sgug_report_last_error(const sgug_reporter *r)
{
	return r != NULL ? r->err : "";
}

/*
 * Deterministic record ids derived from the step id, so a retry addresses the
 * same record instead of creating a duplicate. The service accepts any GUID we
 * choose for step records; only the job record's id is fixed.
 */
static void
derive_record_id(const char *step_id, char *out, size_t outlen)
{
	sgug_snprintf(out, outlen, "%s", step_id);
}

/*
 * Development aid: reports which reporting surface this deployment actually
 * accepts. Enabled with SGUG_PROBE, since it costs a handful of requests and
 * needs a live job token.
 */
static void probe_endpoints(sgug_reporter *r);

sgug_reporter *
sgug_report_new(sgug_http_client *http, const sgug_job *job)
{
	sgug_reporter *r = calloc(1, sizeof(*r));
	size_t i;

	if (r == NULL)
		return NULL;

	r->http = http;
	r->job = job;

	sgug_snprintf(r->auth, sizeof(r->auth), "Authorization: Bearer %s",
	    job->access_token);

	/* PipelinesServiceUrl, not the tenant URL, and planType as the hub. */
	sgug_snprintf(r->base, sizeof(r->base),
	    "%s_apis/distributedtask/hubs/%s/plans/%s",
	    job->pipelines_url[0] != '\0' ? job->pipelines_url : job->service_url,
	    job->plan_type, job->plan_id);

	r->legacy_timeline = job->results_url == NULL ||
	    job->results_url[0] == '\0';
	r->results = sgug_results_new(http, job);

	for (i = 0; i < job->nsteps; i++)
		derive_record_id(job->steps[i].id, r->steps[i].record_id,
		    sizeof(r->steps[i].record_id));

	if (getenv("SGUG_PROBE") != NULL)
		probe_endpoints(r);

	return r;
}

void
sgug_report_free(sgug_reporter *r)
{
	size_t i, j;

	if (r == NULL)
		return;

	sgug_results_free(r->results);

	for (i = 0; i < SGUG_JOB_MAX_STEPS; i++) {
		free(r->steps[i].log);
		for (j = 0; j < r->steps[i].nfeed; j++)
			free(r->steps[i].feed[j]);
	}
	free(r);
}

static int
send(sgug_reporter *r, const char *method, const char *url, const char *ctype,
    const void *body, size_t body_len, sgug_http_resp **out)
{
	const char *headers[3];
	char accept[64];
	sgug_http_resp *resp = NULL;
	size_t nh = 2;

	sgug_snprintf(accept, sizeof(accept),
	    "Accept: application/json; api-version=%s", API);

	headers[0] = r->auth;
	headers[1] = accept;
	if (ctype != NULL)
		headers[nh++] = ctype;

	if (sgug_http_request(r->http, method, url, headers, nh, body, body_len,
	    TIMEOUT_MS, &resp) != 0) {
		sgug_snprintf(r->err, sizeof(r->err), "%s %s: %s", method, url,
		    sgug_http_last_error());
		return -1;
	}

	if (sgug_http_status(resp) >= 300) {
		sgug_snprintf(r->err, sizeof(r->err), "%s returned %d: %.200s",
		    method, sgug_http_status(resp), sgug_http_body(resp, NULL));
		/* Reporting failures were previously silent, so a job could run
		 * to completion with nothing reaching the UI and only the final
		 * event revealing it. */
		fprintf(stderr, "report: %d %s %s\n", sgug_http_status(resp),
		    method, url);
		if (out == NULL)
			sgug_http_resp_free(resp);
		else
			*out = resp;
		return -1;
	}

	if (out != NULL)
		*out = resp;
	else
		sgug_http_resp_free(resp);
	return 0;
}

/*
 * Emits one TimelineRecord. state is Pending, InProgress or Completed; result
 * and the timestamps are omitted when not applicable, since the service treats
 * an explicit null differently from an absent field on a merge.
 */
static void
emit_record(sgug_jsonw *w, const char *id, const char *parent, const char *type,
    const char *name, const char *refname, int order, const char *state,
    const char *result, const char *started, const char *finished,
    int64_t log_id)
{
	char now[40];

	sgug_format_iso8601(sgug_now(), now, sizeof(now));

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "id");
	sgug_jsonw_str(w, id);
	if (parent != NULL) {
		sgug_jsonw_key(w, "parentId");
		sgug_jsonw_str(w, parent);
	}
	sgug_jsonw_key(w, "type");
	sgug_jsonw_str(w, type);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, name);
	if (refname != NULL) {
		sgug_jsonw_key(w, "refName");
		sgug_jsonw_str(w, refname);
	}
	sgug_jsonw_key(w, "order");
	sgug_jsonw_int(w, order);
	sgug_jsonw_key(w, "state");
	sgug_jsonw_str(w, state);
	if (result != NULL) {
		sgug_jsonw_key(w, "result");
		sgug_jsonw_str(w, result);
		sgug_jsonw_key(w, "percentComplete");
		sgug_jsonw_int(w, 100);
	}
	if (started != NULL && started[0] != '\0') {
		sgug_jsonw_key(w, "startTime");
		sgug_jsonw_str(w, started);
	}
	if (finished != NULL && finished[0] != '\0') {
		sgug_jsonw_key(w, "finishTime");
		sgug_jsonw_str(w, finished);
	}
	if (log_id > 0) {
		sgug_jsonw_key(w, "log");
		sgug_jsonw_obj_begin(w);
		sgug_jsonw_key(w, "id");
		sgug_jsonw_int(w, log_id);
		sgug_jsonw_obj_end(w);
	}
	sgug_jsonw_key(w, "lastModified");
	sgug_jsonw_str(w, now);
	sgug_jsonw_key(w, "workerName");
	sgug_jsonw_str(w, "irix");
	sgug_jsonw_obj_end(w);
}

/*
 * Pushes the current state of every step to the results service, which is what
 * makes them light up as the job runs. Without it the UI shows nothing until
 * completejob lands and every step appears at once.
 */
static void
push_steps(sgug_reporter *r)
{
	sgug_step_state states[SGUG_JOB_MAX_STEPS];
	char err[256];
	size_t i;

	if (r->results == NULL || r->job->nsteps == 0)
		return;

	for (i = 0; i < r->job->nsteps; i++) {
		struct step_state *st = &r->steps[i];

		states[i].external_id = st->record_id;
		states[i].name = r->job->steps[i].display_name;
		states[i].number = (int)i + 1;
		states[i].started_at = st->started[0] != '\0' ? st->started : NULL;
		states[i].completed_at =
		    st->finished[0] != '\0' ? st->finished : NULL;

		if (st->ran) {
			states[i].status = SGUG_STEP_DONE;
			states[i].conclusion = result_name(st->result);
		} else if (st->started[0] != '\0') {
			states[i].status = SGUG_STEP_RUNNING;
			states[i].conclusion = NULL;
		} else {
			states[i].status = SGUG_STEP_PENDING;
			states[i].conclusion = NULL;
		}
	}

	/*
	 * Best effort, and disabled after the first refusal.
	 *
	 * This service rejects the plan id as workflow_run_backend_id with
	 * "workflow run not found", even though the receiver service accepts
	 * the same value for log uploads, so the identifier it wants is
	 * something else. Live step state is cosmetic: completejob still
	 * carries accurate per-step results and timestamps, so a failure here
	 * costs the running highlight and nothing else.
	 */
	if (r->steps_update_broken)
		return;

	err[0] = '\0';
	if (sgug_results_steps_update(r->results, states, r->job->nsteps, err,
	    sizeof(err)) != 0) {
		fprintf(stderr, "live step updates unavailable: %s\n", err);
		r->steps_update_broken = 1;
	}
}

/* PATCH is a merge, so only changed records need sending. */
static int
patch_records(sgug_reporter *r, sgug_jsonw *w, int count)
{
	char url[SGUG_MAX_URL];
	char ctype[128];
	const char *body;
	size_t len;

	if (!r->legacy_timeline)
		return 0;

	body = sgug_jsonw_done(w, &len);
	if (body == NULL) {
		sgug_snprintf(r->err, sizeof(r->err), "could not build records");
		return -1;
	}
	(void)count;

	sgug_snprintf(url, sizeof(url), "%s/timelines/%s/records", r->base,
	    r->job->timeline_id);
	sgug_snprintf(ctype, sizeof(ctype),
	    "Content-Type: application/json; charset=utf-8; api-version=%s", API);

	return send(r, "PATCH", url, ctype, body, len, NULL);
}

static sgug_jsonw *
begin_records(int count)
{
	sgug_jsonw *w = sgug_jsonw_new();

	if (w == NULL)
		return NULL;

	/* VssJsonCollectionWrapper. A bare array is rejected. */
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "count");
	sgug_jsonw_int(w, count);
	sgug_jsonw_key(w, "value");
	sgug_jsonw_arr_begin(w);
	return w;
}

static void
end_records(sgug_jsonw *w)
{
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);
}

int
sgug_report_begin(sgug_reporter *r)
{
	sgug_jsonw *w = begin_records((int)r->job->nsteps + 1);
	size_t i;
	int rc;

	if (w == NULL)
		return -1;

	emit_record(w, r->job->job_id, NULL, "Job", r->job->job_display_name,
	    r->job->job_name, 1, "Pending", NULL, NULL, NULL, 0);

	for (i = 0; i < r->job->nsteps; i++) {
		emit_record(w, r->steps[i].record_id, r->job->job_id, "Task",
		    r->job->steps[i].display_name,
		    r->job->steps[i].context_name, (int)i + 1, "Pending",
		    NULL, NULL, NULL, 0);
	}

	end_records(w);
	rc = patch_records(r, w, (int)r->job->nsteps + 1);
	sgug_jsonw_free(w);
	return rc;
}

int
sgug_report_job_started(sgug_reporter *r)
{
	sgug_jsonw *w = begin_records(1);
	int rc;

	if (w == NULL)
		return -1;

	sgug_format_iso8601(sgug_now(), r->job_started, sizeof(r->job_started));
	emit_record(w, r->job->job_id, NULL, "Job", r->job->job_display_name,
	    r->job->job_name, 1, "InProgress", NULL, r->job_started, NULL, 0);

	end_records(w);
	rc = patch_records(r, w, 1);
	sgug_jsonw_free(w);
	return rc;
}

int
sgug_report_step_started(sgug_reporter *r, size_t i)
{
	sgug_jsonw *w;
	int rc;

	if (i >= r->job->nsteps)
		return -1;

	w = begin_records(1);
	if (w == NULL)
		return -1;

	sgug_results_timestamp(sgug_now(), r->steps[i].started,
	    sizeof(r->steps[i].started));
	emit_record(w, r->steps[i].record_id, r->job->job_id, "Task",
	    r->job->steps[i].display_name, r->job->steps[i].context_name,
	    (int)i + 1, "InProgress", NULL, r->steps[i].started, NULL, 0);

	end_records(w);
	rc = patch_records(r, w, 1);
	sgug_jsonw_free(w);
	push_steps(r);
	return rc;
}

/*
 * Replaces every literal secret the service listed. Done on the way out rather
 * than at the point of logging, so nothing can bypass it.
 */
static char *
redact(const sgug_reporter *r, const char *line)
{
	char *out = strdup(line);
	size_t m;

	if (out == NULL)
		return NULL;

	for (m = 0; m < r->job->nmasks; m++) {
		const char *secret = r->job->masks[m];
		size_t slen = strlen(secret);
		char *p;

		if (slen == 0)
			continue;

		while ((p = strstr(out, secret)) != NULL) {
			memmove(p + 3, p + slen, strlen(p + slen) + 1);
			memcpy(p, "***", 3);
		}
	}
	return out;
}

static int
log_append(struct step_state *st, const char *text)
{
	size_t n = strlen(text);

	if (st->log_len + n + 2 > st->log_cap) {
		size_t ncap = st->log_cap != 0 ? st->log_cap : 4096;
		char *nb;

		while (ncap < st->log_len + n + 2)
			ncap *= 2;
		nb = realloc(st->log, ncap);
		if (nb == NULL)
			return -1;
		st->log = nb;
		st->log_cap = ncap;
	}

	memcpy(st->log + st->log_len, text, n);
	st->log_len += n;
	st->log[st->log_len++] = '\n';
	st->log[st->log_len] = '\0';
	st->log_lines++;
	return 0;
}

int
sgug_report_step_output(sgug_reporter *r, size_t i, const char *line)
{
	struct step_state *st;
	char *clean;
	char stamped[MAX_LINE + 48];
	char now[40];

	if (i >= r->job->nsteps)
		return -1;
	st = &r->steps[i];

	clean = redact(r, line);
	if (clean == NULL)
		return -1;

	if (strlen(clean) > MAX_LINE)
		clean[MAX_LINE] = '\0';

	/* The persisted log carries a timestamp per line; the live feed does
	 * not, and the UI adds its own. */
	sgug_format_iso8601(sgug_now(), now, sizeof(now));
	sgug_snprintf(stamped, sizeof(stamped), "%s %s", now, clean);
	log_append(st, stamped);

	if (st->nfeed < MAX_FEED_LINES) {
		st->feed[st->nfeed++] = clean;
	} else {
		/* Live output is best effort. The uploaded log still has it. */
		free(clean);
	}
	return 0;
}

int
sgug_report_flush(sgug_reporter *r)
{
	size_t i, j;
	int rc = 0;

	if (!r->legacy_timeline) {
		/* Drop what we buffered; the console feed lives on the results
		 * service, which is not implemented yet. */
		for (i = 0; i < r->job->nsteps; i++) {
			struct step_state *st = &r->steps[i];

			for (j = 0; j < st->nfeed; j++)
				free(st->feed[j]);
			st->lines_sent += (int64_t)st->nfeed;
			st->nfeed = 0;
		}
		return 0;
	}

	for (i = 0; i < r->job->nsteps; i++) {
		struct step_state *st = &r->steps[i];
		sgug_jsonw *w;
		char url[SGUG_MAX_URL];
		char ctype[128];
		const char *body;
		size_t len;

		if (st->nfeed == 0)
			continue;

		w = sgug_jsonw_new();
		if (w == NULL)
			return -1;

		sgug_jsonw_obj_begin(w);
		sgug_jsonw_key(w, "count");
		sgug_jsonw_int(w, (int64_t)st->nfeed);
		sgug_jsonw_key(w, "value");
		sgug_jsonw_arr_begin(w);
		for (j = 0; j < st->nfeed; j++)
			sgug_jsonw_str(w, st->feed[j]);
		sgug_jsonw_arr_end(w);
		sgug_jsonw_key(w, "stepId");
		sgug_jsonw_str(w, st->record_id);
		sgug_jsonw_key(w, "startLine");
		sgug_jsonw_int(w, st->lines_sent + 1);
		sgug_jsonw_obj_end(w);

		body = sgug_jsonw_done(w, &len);
		if (body != NULL) {
			sgug_snprintf(url, sizeof(url),
			    "%s/timelines/%s/records/%s/feed", r->base,
			    r->job->timeline_id, st->record_id);
			sgug_snprintf(ctype, sizeof(ctype),
			    "Content-Type: application/json; charset=utf-8; "
			    "api-version=%s", API);
			/* Best effort: a dropped console batch must not fail
			 * the job, since the uploaded log is authoritative. */
			if (send(r, "POST", url, ctype, body, len, NULL) != 0)
				rc = 0;
		}

		st->lines_sent += (int64_t)st->nfeed;
		for (j = 0; j < st->nfeed; j++)
			free(st->feed[j]);
		st->nfeed = 0;

		sgug_jsonw_free(w);
	}
	return rc;
}

/* Creates a log container and returns its id, or -1. */
static int64_t
create_log(sgug_reporter *r)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_http_resp *resp = NULL;
	sgug_json_doc *doc;
	char url[SGUG_MAX_URL];
	char ctype[128];
	char path[80];
	char now[40];
	const char *body;
	size_t len;
	int64_t id = -1;

	if (w == NULL)
		return -1;

	if (!r->legacy_timeline) {
		sgug_jsonw_free(w);
		return -1;
	}

	sgug_format_iso8601(sgug_now(), now, sizeof(now));
	sgug_snprintf(path, sizeof(path), "logs/%s", r->job->job_id);

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "path");
	sgug_jsonw_str(w, path);
	sgug_jsonw_key(w, "createdOn");
	sgug_jsonw_str(w, now);
	sgug_jsonw_key(w, "lastChangedOn");
	sgug_jsonw_str(w, now);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &len);
	if (body == NULL) {
		sgug_jsonw_free(w);
		return -1;
	}

	sgug_snprintf(url, sizeof(url), "%s/logs", r->base);
	sgug_snprintf(ctype, sizeof(ctype),
	    "Content-Type: application/json; charset=utf-8; api-version=%s", API);

	if (send(r, "POST", url, ctype, body, len, &resp) == 0) {
		size_t rl;
		const char *rb = sgug_http_body(resp, &rl);

		doc = sgug_json_parse(rb, rl, NULL, 0);
		if (doc != NULL) {
			id = sgug_json_int(
			    sgug_json_get(sgug_json_root(doc), "id"), -1);
			sgug_json_free(doc);
		}
	}

	sgug_http_resp_free(resp);
	sgug_jsonw_free(w);
	return id;
}

int
sgug_report_step_finished(sgug_reporter *r, size_t i, sgug_result result)
{
	struct step_state *st;
	sgug_jsonw *w;
	char url[SGUG_MAX_URL];
	char finished[40];
	int rc;

	if (i >= r->job->nsteps)
		return -1;
	st = &r->steps[i];

	sgug_report_flush(r);

	if (st->log_len > 0 && r->results != NULL) {
		char uerr[256];

		uerr[0] = '\0';
		if (sgug_results_step_log(r->results, st->record_id, st->log,
		    st->log_len, st->log_lines, uerr, sizeof(uerr)) != 0)
			fprintf(stderr, "log upload: %s\n", uerr);
	}

	if (st->log_len > 0 && r->legacy_timeline) {
		st->log_id = create_log(r);
		if (st->log_id > 0) {
			char idbuf[24];

			sgug_i64toa(st->log_id, idbuf, sizeof(idbuf));
			sgug_snprintf(url, sizeof(url), "%s/logs/%s", r->base,
			    idbuf);
			/* Raw bytes, not JSON. */
			send(r, "POST", url,
			    "Content-Type: application/octet-stream",
			    st->log, st->log_len, NULL);
		}
	}

	w = begin_records(1);
	if (w == NULL)
		return -1;

	sgug_results_timestamp(sgug_now(), st->finished, sizeof(st->finished));
	sgug_snprintf(finished, sizeof(finished), "%s", st->finished);
	st->result = result;
	st->ran = 1;
	emit_record(w, st->record_id, r->job->job_id, "Task",
	    r->job->steps[i].display_name, r->job->steps[i].context_name,
	    (int)i + 1, "Completed", result_name(result), st->started,
	    finished, st->log_id > 0 ? st->log_id : 0);

	end_records(w);
	rc = patch_records(r, w, 1);
	sgug_jsonw_free(w);
	push_steps(r);
	return rc;
}

int
sgug_report_job_finished(sgug_reporter *r, sgug_result result)
{
	sgug_jsonw *w;
	char url[SGUG_MAX_URL];
	char ctype[128];
	char finished[40];
	const char *body;
	size_t len, i;
	int rc;

	w = begin_records(1);
	if (w == NULL)
		return -1;

	sgug_format_iso8601(sgug_now(), finished, sizeof(finished));
	emit_record(w, r->job->job_id, NULL, "Job", r->job->job_display_name,
	    r->job->job_name, 1, "Completed", result_name(result),
	    r->job_started, finished, 0);
	end_records(w);
	rc = patch_records(r, w, 1);
	sgug_jsonw_free(w);

	/*
	 * The job log is the concatenation of the step logs, uploaded
	 * separately from them. The steps drive what each section shows when
	 * expanded; this is what the download button and the REST logs endpoint
	 * serve, and without it that endpoint answers BlobNotFound even though
	 * the steps look complete.
	 */
	if (r->results != NULL) {
		char *joblog = NULL;
		size_t total = 0, off = 0;
		int64_t lines = 0;

		for (i = 0; i < r->job->nsteps; i++) {
			total += r->steps[i].log_len;
			lines += r->steps[i].log_lines;
		}

		if (total > 0)
			joblog = malloc(total + 1);
		if (joblog != NULL) {
			char uerr[256];

			for (i = 0; i < r->job->nsteps; i++) {
				if (r->steps[i].log_len == 0)
					continue;
				memcpy(joblog + off, r->steps[i].log,
				    r->steps[i].log_len);
				off += r->steps[i].log_len;
			}
			joblog[off] = '\0';

			uerr[0] = '\0';
			if (sgug_results_job_log(r->results, joblog, off, lines,
			    uerr, sizeof(uerr)) != 0)
				fprintf(stderr, "job log upload: %s\n", uerr);
			free(joblog);
		}
	}

	/*
	 * Completion goes to the run service, not the plan events route.
	 *
	 * Modern github.com does not serve the Azure DevOps timeline API for
	 * these plans at all: the official runner v2.336.0 contains no
	 * _apis/distributedtask routes, only run-service verbs (session,
	 * message, acquirejob, renewjob, completejob, acknowledge) and a Twirp
	 * results service. Every timeline call here returns 404, so step state
	 * is carried in this payload instead.
	 *
	 * This is also what releases the parallelism slot. A runner that never
	 * sends it leaves the agent stuck at currentParallelism 1 and
	 * undispatchable while still reporting online.
	 */
	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "planId");
	sgug_jsonw_str(w, r->job->plan_id);
	sgug_jsonw_key(w, "jobId");
	sgug_jsonw_str(w, r->job->job_id);
	sgug_jsonw_key(w, "conclusion");
	sgug_jsonw_str(w, result_name(result));

	sgug_jsonw_key(w, "stepResults");
	sgug_jsonw_arr_begin(w);
	for (i = 0; i < r->job->nsteps; i++) {
		struct step_state *st = &r->steps[i];

		if (!st->ran)
			continue;

		sgug_jsonw_obj_begin(w);
		sgug_jsonw_key(w, "external_id");
		sgug_jsonw_str(w, st->record_id);
		sgug_jsonw_key(w, "number");
		sgug_jsonw_int(w, (int64_t)i + 1);
		sgug_jsonw_key(w, "name");
		sgug_jsonw_str(w, r->job->steps[i].display_name);
		sgug_jsonw_key(w, "status");
		sgug_jsonw_str(w, "completed");
		sgug_jsonw_key(w, "conclusion");
		sgug_jsonw_str(w, result_name(st->result));
		/*
		 * Omitted rather than sent empty. A skipped step never started,
		 * so it has no timestamp, and an empty string fails the
		 * service's timestamp parse: completejob answers 400, the job
		 * never completes, and the runner keeps its parallelism slot
		 * forever. One skipped step was enough to wedge the runner.
		 */
		if (st->started[0] != '\0') {
			sgug_jsonw_key(w, "started_at");
			sgug_jsonw_str(w, st->started);
		}
		if (st->finished[0] != '\0') {
			sgug_jsonw_key(w, "completed_at");
			sgug_jsonw_str(w, st->finished);
		}
		sgug_jsonw_key(w, "completed_log_url");
		sgug_jsonw_null(w);
		sgug_jsonw_key(w, "completed_log_lines");
		sgug_jsonw_int(w, st->log_lines);
		sgug_jsonw_key(w, "annotations");
		sgug_jsonw_arr_begin(w);
		sgug_jsonw_arr_end(w);
		sgug_jsonw_obj_end(w);
	}
	sgug_jsonw_arr_end(w);

	sgug_jsonw_key(w, "annotations");
	sgug_jsonw_arr_begin(w);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_key(w, "environmentUrl");
	sgug_jsonw_null(w);
	sgug_jsonw_key(w, "billingOwnerId");
	sgug_jsonw_str(w, r->job->billing_owner_id);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &len);
	if (body == NULL) {
		sgug_jsonw_free(w);
		return -1;
	}

	/* The run service takes no api-version. */
	sgug_snprintf(url, sizeof(url), "%scompletejob", r->job->service_url);
	sgug_snprintf(ctype, sizeof(ctype),
	    "Content-Type: application/json; charset=utf-8");

	rc = send(r, "POST", url, ctype, body, len, NULL);
	sgug_jsonw_free(w);
	return rc;
}

static void
probe_endpoints(sgug_reporter *r)
{
	static const char *const HUBS[] = { "actions", "Actions", "pipelines" };
	const char *bases[2];
	char url[SGUG_MAX_URL];
	const char *headers[2];
	char accept[64];
	size_t b, h;

	bases[0] = r->job->pipelines_url;
	bases[1] = r->job->service_url;

	sgug_snprintf(accept, sizeof(accept),
	    "Accept: application/json; api-version=%s", API);
	headers[0] = r->auth;
	headers[1] = accept;

	fprintf(stderr, "probe: planId=%s timelineId=%s\n", r->job->plan_id,
	    r->job->timeline_id);

	for (b = 0; b < 2; b++) {
		if (bases[b] == NULL || bases[b][0] == '\0')
			continue;
		for (h = 0; h < sizeof(HUBS) / sizeof(HUBS[0]); h++) {
			sgug_http_resp *resp = NULL;

			sgug_snprintf(url, sizeof(url),
			    "%s_apis/distributedtask/hubs/%s/plans/%s/timelines/%s",
			    bases[b], HUBS[h], r->job->plan_id,
			    r->job->timeline_id);

			if (sgug_http_request(r->http, "GET", url, headers, 2,
			    NULL, 0, TIMEOUT_MS, &resp) == 0) {
				fprintf(stderr, "probe: %d base=%lu hub=%s\n",
				    sgug_http_status(resp), (unsigned long)b,
				    HUBS[h]);
				sgug_http_resp_free(resp);
			} else {
				fprintf(stderr, "probe: transport fail base=%lu "
				    "hub=%s\n", (unsigned long)b, HUBS[h]);
			}
		}
	}
	fflush(stderr);
}
