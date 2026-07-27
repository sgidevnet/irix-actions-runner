#include "proto/results.h"

#include "json/json.h"
#include "proto/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECEIVER "twirp/results.services.receiver.Receiver/"
#define TIMEOUT_MS 60000

struct sgug_results {
	sgug_http_client *http;
	const sgug_job *job;
	char auth[4096];
	char base[SGUG_MAX_URL];
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

sgug_results *
sgug_results_new(sgug_http_client *http, const sgug_job *job)
{
	sgug_results *r;
	size_t n;

	if (job->results_url == NULL || job->results_url[0] == '\0')
		return NULL;

	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return NULL;

	r->http = http;
	r->job = job;

	sgug_snprintf(r->auth, sizeof(r->auth), "Authorization: Bearer %s",
	    job->access_token);

	n = strlen(job->results_url);
	if (n > 0 && job->results_url[n - 1] == '/')
		sgug_snprintf(r->base, sizeof(r->base), "%s", job->results_url);
	else
		sgug_snprintf(r->base, sizeof(r->base), "%s/", job->results_url);

	return r;
}

void
sgug_results_free(sgug_results *r)
{
	free(r);
}

/*
 * The two identifiers the service calls "backend ids" are ones we already
 * hold: the run is the plan, the job run is the job.
 */
static void
emit_ids(sgug_jsonw *w, const sgug_job *job, const char *step_id)
{
	sgug_jsonw_key(w, "workflow_job_run_backend_id");
	sgug_jsonw_str(w, job->job_id);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, job->plan_id);
	if (step_id != NULL) {
		sgug_jsonw_key(w, "step_backend_id");
		sgug_jsonw_str(w, step_id);
	}
}

static int
twirp(sgug_results *r, const char *method, const char *body, size_t body_len,
    sgug_http_resp **out, char *err, size_t errlen)
{
	char url[SGUG_MAX_URL];
	const char *headers[3];
	sgug_http_resp *resp = NULL;

	sgug_snprintf(url, sizeof(url), "%s%s%s", r->base, RECEIVER, method);

	headers[0] = r->auth;
	headers[1] = "Accept: application/json";
	headers[2] = "Content-Type: application/json";

	if (sgug_http_request(r->http, "POST", url, headers, 3, body, body_len,
	    TIMEOUT_MS, &resp) != 0) {
		seterr(err, errlen, "%s: %s", method, sgug_http_last_error());
		return -1;
	}

	if (sgug_http_status(resp) >= 300) {
		seterr(err, errlen, "%s returned %d: %.180s", method,
		    sgug_http_status(resp), sgug_http_body(resp, NULL));
		sgug_http_resp_free(resp);
		return -1;
	}

	if (out != NULL)
		*out = resp;
	else
		sgug_http_resp_free(resp);
	return 0;
}

/*
 * Reads a field under either spelling. Protobuf JSON may arrive as the proto
 * name or as its lowerCamelCase form depending on how the service serialises,
 * and the two differ by more than case so one lookup will not find both.
 */
static const char *
field(const sgug_json *o, const char *snake, const char *camel)
{
	const char *v = sgug_json_string(sgug_json_get(o, snake), NULL);

	if (v == NULL)
		v = sgug_json_string(sgug_json_get(o, camel), NULL);
	return v;
}

static int
upload(sgug_results *r, const char *step_id, const char *geturl_method,
    const char *meta_method, const char *log, size_t len, int64_t line_count,
    char *err, size_t errlen)
{
	sgug_jsonw *w = NULL;
	sgug_http_resp *resp = NULL;
	sgug_json_doc *doc = NULL;
	const char *body, *signed_url;
	char stamp[40];
	char lines[24];
	size_t body_len;
	int rc = -1;

	if (r == NULL || len == 0)
		return 0;

	/* 1. Ask for somewhere to put it. */
	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;
	sgug_jsonw_obj_begin(w);
	emit_ids(w, r->job, step_id);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	if (twirp(r, geturl_method, body, body_len, &resp, err, errlen) != 0)
		goto out;

	{
		size_t rl;
		const char *rb = sgug_http_body(resp, &rl);

		doc = sgug_json_parse(rb, rl, NULL, 0);
	}
	if (doc == NULL) {
		seterr(err, errlen, "signed URL response was not JSON");
		goto out;
	}

	signed_url = field(sgug_json_root(doc), "logs_url", "logsUrl");
	if (signed_url == NULL || *signed_url == '\0') {
		seterr(err, errlen, "signed URL response carried no logs_url");
		goto out;
	}

	/*
	 * 2. PUT the bytes. No Authorization header: the URL is already a
	 * signed SAS, and sending a bearer token alongside it is rejected.
	 * BlockBlob uploads the whole log at once; AppendBlob would be the
	 * streaming variant.
	 */
	{
		const char *blob_headers[2];
		sgug_http_resp *put = NULL;

		blob_headers[0] = "x-ms-blob-type: BlockBlob";
		blob_headers[1] = "Content-Type: text/plain";

		if (sgug_http_request(r->http, "PUT", signed_url, blob_headers, 2,
		    log, len, TIMEOUT_MS, &put) != 0) {
			seterr(err, errlen, "blob upload: %s",
			    sgug_http_last_error());
			goto out;
		}
		if (sgug_http_status(put) >= 300) {
			seterr(err, errlen, "blob upload returned %d",
			    sgug_http_status(put));
			sgug_http_resp_free(put);
			goto out;
		}
		sgug_http_resp_free(put);
	}

	sgug_json_free(doc);
	doc = NULL;
	sgug_http_resp_free(resp);
	resp = NULL;
	sgug_jsonw_free(w);

	/* 3. Register it, which is what makes the log appear. */
	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_format_iso8601(sgug_now(), stamp, sizeof(stamp));
	sgug_i64toa(line_count, lines, sizeof(lines));

	sgug_jsonw_obj_begin(w);
	emit_ids(w, r->job, step_id);
	sgug_jsonw_key(w, "uploaded_at");
	sgug_jsonw_str(w, stamp);
	/* Sent as a string: protobuf JSON encodes 64-bit integers that way, and
	 * a bare number is rejected by some builds of the service. */
	sgug_jsonw_key(w, "line_count");
	sgug_jsonw_str(w, lines);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	if (twirp(r, meta_method, body, body_len, NULL, err, errlen) != 0)
		goto out;

	rc = 0;

out:
	sgug_json_free(doc);
	sgug_http_resp_free(resp);
	sgug_jsonw_free(w);
	return rc;
}

int
sgug_results_step_log(sgug_results *r, const char *step_id, const char *log,
    size_t len, int64_t line_count, char *err, size_t errlen)
{
	return upload(r, step_id, "GetStepLogsSignedBlobURL",
	    "CreateStepLogsMetadata", log, len, line_count, err, errlen);
}

int
sgug_results_job_log(sgug_results *r, const char *log, size_t len,
    int64_t line_count, char *err, size_t errlen)
{
	/* No step id: the job-level request carries only the two run ids. */
	return upload(r, NULL, "GetJobLogsSignedBlobURL",
	    "CreateJobLogsMetadata", log, len, line_count, err, errlen);
}
