#include "proto/results.h"

#include "json/json.h"

#include <openssl/evp.h>
#include <stdio.h>
#include "proto/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECEIVER "twirp/results.services.receiver.Receiver/"
#define STEPSVC "twirp/github.actions.results.api.v1.WorkflowStepUpdateService/"
#define ARTSVC "twirp/github.actions.results.api.v1.ArtifactService/"

/* The artifact format version the service expects for v4 artifacts. */
#define ARTIFACT_VERSION 7
#define TIMEOUT_MS 60000

struct sgug_results {
	sgug_http_client *http;
	const sgug_job *job;
	char auth[4096];
	char base[SGUG_MAX_URL];
	/* Monotonic across the job; the service drops out-of-order updates. */
	int64_t change_order;
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
	sgug_jsonw_str(w, job->backend_job_id);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, job->backend_run_id);
	if (step_id != NULL) {
		sgug_jsonw_key(w, "step_backend_id");
		sgug_jsonw_str(w, step_id);
	}
}

void
sgug_results_timestamp(sgug_time_t t, char *out, size_t outlen)
{
	char full[40];
	size_t n;

	/* The shared helper emits seven fractional digits for the timeline API;
	 * this service wants three. Trim rather than reformat. */
	sgug_format_iso8601(t, full, sizeof(full));
	n = strlen(full);
	if (n >= 12) {
		full[n - 5] = 'Z';
		full[n - 4] = '\0';
	}
	sgug_snprintf(out, outlen, "%s", full);
}

/*
 * Enum numbers, not names. A name fails the service's decoder, which then
 * zeroes the request and answers 404 on the run id. Values are non-sequential.
 */
static int
status_value(sgug_step_status s)
{
	switch (s) {
	case SGUG_STEP_RUNNING: return 3;
	case SGUG_STEP_DONE: return 6;
	default: return 5;
	}
}

static int
conclusion_value(const char *result)
{
	if (result == NULL)
		return 0;
	if (strcmp(result, "succeeded") == 0)
		return 2;
	if (strcmp(result, "failed") == 0)
		return 3;
	if (strcmp(result, "canceled") == 0)
		return 4;
	if (strcmp(result, "skipped") == 0)
		return 7;
	return 0;
}

static int
twirp_at(sgug_results *r, const char *svc, const char *method,
    const char *body, size_t body_len, sgug_http_resp **out, char *err,
    size_t errlen);

int
sgug_results_steps_update(sgug_results *r, const sgug_step_state *steps,
    size_t nsteps, char *err, size_t errlen)
{
	sgug_jsonw *w;
	const char *body;
	size_t body_len, i;
	int rc;

	if (r == NULL || nsteps == 0)
		return 0;

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_run_id);
	sgug_jsonw_key(w, "workflow_job_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_job_id);
	sgug_jsonw_key(w, "change_order");
	sgug_jsonw_int(w, ++r->change_order);

	sgug_jsonw_key(w, "steps");
	sgug_jsonw_arr_begin(w);
	for (i = 0; i < nsteps; i++) {
		const sgug_step_state *st = &steps[i];

		sgug_jsonw_obj_begin(w);
		sgug_jsonw_key(w, "external_id");
		sgug_jsonw_str(w, st->external_id);
		sgug_jsonw_key(w, "number");
		sgug_jsonw_int(w, st->number);
		sgug_jsonw_key(w, "name");
		sgug_jsonw_str(w, st->name);
		sgug_jsonw_key(w, "status");
		sgug_jsonw_int(w, status_value(st->status));
		sgug_jsonw_key(w, "conclusion");
		sgug_jsonw_int(w, st->status == SGUG_STEP_DONE
		    ? conclusion_value(st->conclusion) : 0);
		if (st->started_at != NULL && st->started_at[0] != '\0') {
			sgug_jsonw_key(w, "started_at");
			sgug_jsonw_str(w, st->started_at);
		}
		if (st->completed_at != NULL && st->completed_at[0] != '\0') {
			sgug_jsonw_key(w, "completed_at");
			sgug_jsonw_str(w, st->completed_at);
		}
		sgug_jsonw_obj_end(w);
	}
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL) {
		sgug_jsonw_free(w);
		return -1;
	}

	rc = twirp_at(r, STEPSVC, "WorkflowStepsUpdate", body, body_len, NULL,
	    err, errlen);
	sgug_jsonw_free(w);
	return rc;
}

static int
twirp_at(sgug_results *r, const char *svc, const char *method,
    const char *body, size_t body_len, sgug_http_resp **out, char *err,
    size_t errlen)
{
	char url[SGUG_MAX_URL];
	const char *headers[3];
	sgug_http_resp *resp = NULL;

	sgug_snprintf(url, sizeof(url), "%s%s%s", r->base, svc, method);

	headers[0] = r->auth;
	headers[1] = "Accept: application/json";
	headers[2] = "Content-Type: application/json";

	if (sgug_http_request(r->http, "POST", url, headers, 3, body, body_len,
	    TIMEOUT_MS, &resp) != 0) {
		seterr(err, errlen, "%s: %s", method, sgug_http_last_error());
		return -1;
	}

	if (sgug_http_status(resp) >= 300) {
		seterr(err, errlen, "%s returned %d: %.400s", method,
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

/* The receiver service carries the logs; the step service is separate. */
static int
twirp(sgug_results *r, const char *method, const char *body, size_t body_len,
    sgug_http_resp **out, char *err, size_t errlen)
{
	return twirp_at(r, RECEIVER, method, body, body_len, out, err, errlen);
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

/*
 * PUTs one block to a signed blob URL.
 *
 * A whole log in one block is a plain BlockBlob. Otherwise it is an append
 * blob: the first block creates it empty, every block extends it, and the last
 * seals it so the service knows nothing more is coming. The signed URL already
 * carries a query string, hence the & rather than ?.
 */
static int
put_block(sgug_results *r, const char *url, const char *data, size_t len,
    int first_block, int finalize, char *err, size_t errlen)
{
	const char *headers[3];
	sgug_http_resp *resp = NULL;
	/*
	 * Not SGUG_MAX_URL. A signed blob URL carries the whole SAS in its
	 * query string and already runs to several hundred bytes, and the
	 * signature covers what follows: truncating it does not fail loudly,
	 * it fails as AuthenticationFailed from Azure.
	 */
	char appendurl[2048];
	size_t want;
	int rc;

	if (first_block && finalize) {
		headers[0] = "x-ms-blob-type: BlockBlob";
		headers[1] = "Content-Type: text/plain";

		if (sgug_http_request(r->http, "PUT", url, headers, 2, data,
		    len, TIMEOUT_MS, &resp) != 0) {
			seterr(err, errlen, "log upload: %s",
			    sgug_http_last_error());
			return -1;
		}
		rc = sgug_http_status(resp);
		if (rc >= 300) {
			seterr(err, errlen, "log upload returned %d: %.200s",
			    rc, sgug_http_body(resp, NULL));
			sgug_http_resp_free(resp);
			return -1;
		}
		sgug_http_resp_free(resp);
		return 0;
	}

	if (first_block) {
		headers[0] = "x-ms-blob-type: AppendBlob";
		headers[1] = "Content-Type: text/plain";

		/* Creates the blob and nothing else; the body must be empty. */
		if (sgug_http_request(r->http, "PUT", url, headers, 2, NULL, 0,
		    TIMEOUT_MS, &resp) != 0) {
			seterr(err, errlen, "append blob create: %s",
			    sgug_http_last_error());
			return -1;
		}
		rc = sgug_http_status(resp);
		if (rc >= 300) {
			seterr(err, errlen, "append blob create returned %d: "
			    "%.200s", rc, sgug_http_body(resp, NULL));
			sgug_http_resp_free(resp);
			return -1;
		}
		sgug_http_resp_free(resp);
		resp = NULL;
	}

	want = strlen(url) + sizeof("&comp=appendblock&seal=true");
	if (want > sizeof(appendurl)) {
		seterr(err, errlen, "signed url is %lu bytes, too long to "
		    "append to", (unsigned long)strlen(url));
		return -1;
	}
	sgug_snprintf(appendurl, sizeof(appendurl), "%s&comp=appendblock%s",
	    url, finalize ? "&seal=true" : "");

	headers[0] = "Content-Type: text/plain";
	headers[1] = finalize ? "x-ms-blob-sealed: True"
	    : "x-ms-blob-sealed: False";

	if (sgug_http_request(r->http, "PUT", appendurl, headers, 2, data, len,
	    TIMEOUT_MS, &resp) != 0) {
		seterr(err, errlen, "append block: %s", sgug_http_last_error());
		return -1;
	}
	rc = sgug_http_status(resp);
	if (rc >= 300) {
		/* Azure explains itself in the body, and the code alone is not
		 * enough to tell a permissions problem from a bad header. */
		seterr(err, errlen, "append block returned %d: %.200s", rc,
		    sgug_http_body(resp, NULL));
		sgug_http_resp_free(resp);
		return -1;
	}
	sgug_http_resp_free(resp);
	return 0;
}

static int
upload(sgug_results *r, const char *step_id, const char *geturl_method,
    const char *meta_method, const char *log, size_t len, int64_t line_count,
    int first_block, int finalize, char *err, size_t errlen)
{
	sgug_jsonw *w = NULL;
	sgug_http_resp *resp = NULL;
	sgug_json_doc *doc = NULL;
	const char *body, *signed_url;
	char stamp[40];
	char lines[24];
	size_t body_len;
	int rc = -1;

	/*
	 * An empty non-final block is nothing to say. An empty final one still
	 * has to go, or a log whose last block landed exactly on the boundary
	 * is never sealed and never registered.
	 */
	if (r == NULL || (len == 0 && !finalize))
		return 0;

	/* 1. Ask for somewhere to put it. A fresh signed URL per block: they
	 * expire, and the blob they name is the same one each time. */
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
	 */
	if (put_block(r, signed_url, log, len, first_block, finalize, err,
	    errlen) != 0)
		goto out;

	sgug_json_free(doc);
	doc = NULL;
	sgug_http_resp_free(resp);
	resp = NULL;
	sgug_jsonw_free(w);
	w = NULL;

	/*
	 * 3. Register it, which is what makes the log appear. Only once, on the
	 * final block: the service counts this as the log being complete.
	 */
	if (!finalize) {
		rc = 0;
		goto out;
	}

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
    size_t len, int64_t line_count, int first_block, int finalize, char *err,
    size_t errlen)
{
	return upload(r, step_id, "GetStepLogsSignedBlobURL",
	    "CreateStepLogsMetadata", log, len, line_count, first_block,
	    finalize, err, errlen);
}

int
sgug_results_job_log(sgug_results *r, const char *log, size_t len,
    int64_t line_count, int first_block, int finalize, char *err, size_t errlen)
{
	/* No step id: the job-level request carries only the two run ids. */
	return upload(r, NULL, "GetJobLogsSignedBlobURL",
	    "CreateJobLogsMetadata", log, len, line_count, first_block,
	    finalize, err, errlen);
}

/* sha256 of a file, lowercase hex. The service compares it on finalize. */
static int
sha256_file(const char *path, char *hex, size_t hexlen, int64_t *size)
{
	static const char HEXD[] = "0123456789abcdef";
	unsigned char buf[16384];
	unsigned char md[32];
	unsigned int mdlen = 0;
	EVP_MD_CTX *ctx;
	FILE *f;
	size_t n;
	int i;

	if (hexlen < 65)
		return -1;

	f = fopen(path, "rb");
	if (f == NULL)
		return -1;

	ctx = EVP_MD_CTX_new();
	if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(ctx);
		fclose(f);
		return -1;
	}

	*size = 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		EVP_DigestUpdate(ctx, buf, n);
		*size += (int64_t)n;
	}
	fclose(f);

	EVP_DigestFinal_ex(ctx, md, &mdlen);
	EVP_MD_CTX_free(ctx);

	for (i = 0; i < (int)mdlen; i++) {
		hex[i * 2] = HEXD[md[i] >> 4];
		hex[i * 2 + 1] = HEXD[md[i] & 0x0f];
	}
	hex[mdlen * 2] = '\0';
	return 0;
}

static char *
slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	*len = (size_t)n;
	return buf;
}

int
sgug_artifact_upload(sgug_results *r, const char *name, const char *zip_path,
    char *err, size_t errlen)
{
	sgug_jsonw *w = NULL;
	sgug_http_resp *resp = NULL;
	sgug_json_doc *doc = NULL;
	char hex[72];
	char hashval[80];
	char sizebuf[24];
	const char *body, *upload_url;
	char *zip = NULL;
	size_t body_len, zip_len = 0;
	int64_t size = 0;
	int rc = -1;

	if (r == NULL)
		return -1;

	if (sha256_file(zip_path, hex, sizeof(hex), &size) != 0) {
		seterr(err, errlen, "cannot read %s", zip_path);
		return -1;
	}

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_run_id);
	sgug_jsonw_key(w, "workflow_job_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_job_id);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, name);
	sgug_jsonw_key(w, "version");
	sgug_jsonw_int(w, ARTIFACT_VERSION);
	/*
	 * Required, despite being a wrapper type that reads as optional in the
	 * generated client. Omitting it fails with "a valid mime_type is
	 * required for this artifact". A protobuf StringValue serialises as a
	 * bare string in Twirp JSON.
	 */
	sgug_jsonw_key(w, "mime_type");
	sgug_jsonw_str(w, "application/zip");
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	if (twirp_at(r, ARTSVC, "CreateArtifact", body, body_len, &resp, err,
	    errlen) != 0)
		goto out;

	{
		size_t rl;
		const char *rb = sgug_http_body(resp, &rl);

		doc = sgug_json_parse(rb, rl, NULL, 0);
	}
	upload_url = doc != NULL
	    ? field(sgug_json_root(doc), "signed_upload_url", "signedUploadUrl")
	    : NULL;
	if (upload_url == NULL || *upload_url == '\0') {
		seterr(err, errlen, "CreateArtifact returned no upload URL");
		goto out;
	}

	zip = slurp(zip_path, &zip_len);
	if (zip == NULL) {
		seterr(err, errlen, "cannot read %s", zip_path);
		goto out;
	}

	{
		const char *bh[2];
		sgug_http_resp *put = NULL;

		bh[0] = "x-ms-blob-type: BlockBlob";
		bh[1] = "Content-Type: application/zip";

		if (sgug_http_request(r->http, "PUT", upload_url, bh, 2, zip,
		    zip_len, TIMEOUT_MS, &put) != 0) {
			seterr(err, errlen, "artifact upload: %s",
			    sgug_http_last_error());
			goto out;
		}
		if (sgug_http_status(put) >= 300) {
			seterr(err, errlen, "artifact upload returned %d",
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

	w = sgug_jsonw_new();
	if (w == NULL)
		goto out;

	sgug_i64toa(size, sizebuf, sizeof(sizebuf));
	sgug_snprintf(hashval, sizeof(hashval), "sha256:%s", hex);

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_run_id);
	sgug_jsonw_key(w, "workflow_job_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_job_id);
	sgug_jsonw_key(w, "name");
	sgug_jsonw_str(w, name);
	sgug_jsonw_key(w, "size");
	sgug_jsonw_str(w, sizebuf);
	sgug_jsonw_key(w, "hash");
	sgug_jsonw_str(w, hashval);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	if (twirp_at(r, ARTSVC, "FinalizeArtifact", body, body_len, NULL, err,
	    errlen) != 0)
		goto out;

	rc = 0;

out:
	free(zip);
	sgug_json_free(doc);
	sgug_http_resp_free(resp);
	sgug_jsonw_free(w);
	return rc;
}

int
sgug_artifact_download(sgug_results *r, const char *name, const char *zip_path,
    char *err, size_t errlen)
{
	sgug_jsonw *w = NULL;
	sgug_http_resp *resp = NULL, *get = NULL;
	sgug_json_doc *doc = NULL;
	const char *body, *url, *data;
	size_t body_len, dlen;
	FILE *f;
	int rc = -1;

	if (r == NULL)
		return -1;

	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "workflow_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_run_id);
	sgug_jsonw_key(w, "workflow_job_run_backend_id");
	sgug_jsonw_str(w, r->job->backend_job_id);
	sgug_jsonw_key(w, "name_filter");
	sgug_jsonw_str(w, name);
	sgug_jsonw_obj_end(w);

	body = sgug_jsonw_done(w, &body_len);
	if (body == NULL)
		goto out;

	/*
	 * List first, because the signed URL request is scoped to the job that
	 * uploaded the artifact, not to the job asking for it. A verify job
	 * asking with its own job id gets "artifact not found" even when the
	 * run id matches. The listing is scoped to the run and reports which
	 * job each artifact belongs to.
	 */
	if (twirp_at(r, ARTSVC, "ListArtifacts", body, body_len, &resp, err,
	    errlen) != 0)
		goto out;

	{
		size_t rl;
		const char *rb = sgug_http_body(resp, &rl);
		const sgug_json *list;
		const char *owner_job = NULL, *owner_run = NULL;
		size_t i, n;

		doc = sgug_json_parse(rb, rl, NULL, 0);
		if (doc == NULL) {
			seterr(err, errlen, "ListArtifacts returned non-JSON");
			goto out;
		}

		list = sgug_json_get(sgug_json_root(doc), "artifacts");
		n = sgug_json_len(list);
		for (i = 0; i < n; i++) {
			const sgug_json *a = sgug_json_at(list, i);
			const char *an = field(a, "name", "name");

			if (an != NULL && strcmp(an, name) == 0) {
				owner_job = field(a, "workflow_job_run_backend_id",
				    "workflowJobRunBackendId");
				owner_run = field(a, "workflow_run_backend_id",
				    "workflowRunBackendId");
				break;
			}
		}

		if (owner_job == NULL) {
			seterr(err, errlen,
			    "no artifact named \"%s\" in this run", name);
			goto out;
		}

		sgug_jsonw_free(w);
		w = sgug_jsonw_new();
		if (w == NULL)
			goto out;
		sgug_jsonw_obj_begin(w);
		/*
		 * Both ids come from the listed artifact, not from our own
		 * token. The toolkit does the same: the signed URL is scoped to
		 * whoever uploaded, and a downloader that substitutes its own
		 * ids gets "artifact not found".
		 */
		sgug_jsonw_key(w, "workflow_run_backend_id");
		sgug_jsonw_str(w, owner_run != NULL ? owner_run
		    : r->job->backend_run_id);
		sgug_jsonw_key(w, "workflow_job_run_backend_id");
		sgug_jsonw_str(w, owner_job);
		sgug_jsonw_key(w, "name");
		sgug_jsonw_str(w, name);
		sgug_jsonw_obj_end(w);

		body = sgug_jsonw_done(w, &body_len);
		if (body == NULL)
			goto out;
	}

	sgug_json_free(doc);
	doc = NULL;
	sgug_http_resp_free(resp);
	resp = NULL;

	if (twirp_at(r, ARTSVC, "GetSignedArtifactURL", body, body_len, &resp,
	    err, errlen) != 0)
		goto out;

	{
		size_t rl;
		const char *rb = sgug_http_body(resp, &rl);

		doc = sgug_json_parse(rb, rl, NULL, 0);
	}
	url = doc != NULL
	    ? field(sgug_json_root(doc), "signed_url", "signedUrl") : NULL;
	if (url == NULL || *url == '\0') {
		seterr(err, errlen, "no signed URL for artifact \"%s\"", name);
		goto out;
	}

	if (sgug_http_request(r->http, "GET", url, NULL, 0, NULL, 0, TIMEOUT_MS,
	    &get) != 0) {
		seterr(err, errlen, "artifact download: %s",
		    sgug_http_last_error());
		goto out;
	}
	if (sgug_http_status(get) >= 300) {
		seterr(err, errlen, "artifact download returned %d",
		    sgug_http_status(get));
		goto out;
	}

	data = sgug_http_body(get, &dlen);
	f = fopen(zip_path, "wb");
	if (f == NULL) {
		seterr(err, errlen, "cannot write %s", zip_path);
		goto out;
	}
	if (fwrite(data, 1, dlen, f) != dlen) {
		fclose(f);
		seterr(err, errlen, "short write to %s", zip_path);
		goto out;
	}
	fclose(f);

	rc = 0;

out:
	sgug_http_resp_free(get);
	sgug_json_free(doc);
	sgug_http_resp_free(resp);
	sgug_jsonw_free(w);
	return rc;
}
