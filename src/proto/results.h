#ifndef SGUG_PROTO_RESULTS_H
#define SGUG_PROTO_RESULTS_H

#include "exec/job.h"
#include "net/http.h"

#include <stddef.h>

/*
 * The results service, which is where job logs actually live.
 *
 * Modern github.com does not serve the Azure DevOps timeline or log APIs for
 * these plans, so a step's output reaches the UI only through here. Without it
 * steps appear with the right names and results but cannot be expanded.
 *
 * Twirp over JSON, relative to the job's ResultsServiceUrl:
 *
 *   twirp/results.services.receiver.Receiver/GetStepLogsSignedBlobURL
 *   twirp/results.services.receiver.Receiver/CreateStepLogsMetadata
 *
 * Upload is three calls: ask for a signed blob URL, PUT the bytes to it, then
 * register the upload. The blob PUT goes to Azure and must not carry our
 * bearer token, since the URL is already a signed SAS.
 */

typedef struct sgug_results sgug_results;

/* Borrows job, which must outlive the returned object. Returns NULL when the
 * job carries no ResultsServiceUrl, which is how an older deployment signals
 * that the legacy timeline API applies instead. */
sgug_results *sgug_results_new(sgug_http_client *http, const sgug_job *job);
void sgug_results_free(sgug_results *r);

/*
 * Uploads one block of a step's log.
 *
 * A log that fits in a single block goes up as one blob: pass first_block and
 * finalize together. Anything longer is streamed, so the runner never has to
 * hold a whole build's output in memory. The first block creates an append
 * blob, later ones extend it, and the last seals it. line_count is the running
 * total and is only read on the final block, which is also the only one that
 * registers the log with the service.
 */
int sgug_results_step_log(sgug_results *r, const char *step_id,
    const char *log, size_t len, int64_t line_count, int first_block,
    int finalize, char *err, size_t errlen);

/*
 * Uploads a block of the whole-job log. Separate from the per-step logs and not
 * derived from them: the steps drive what each section shows when expanded,
 * while this is what the download-log button and the REST logs endpoint serve.
 * Without it that endpoint answers BlobNotFound even though the steps look
 * complete. Blocking works as for a step log.
 */
int sgug_results_job_log(sgug_results *r, const char *log, size_t len,
    int64_t line_count, int first_block, int finalize, char *err,
    size_t errlen);

/* Live step state, mirroring what the UI shows while a job runs. */
typedef enum {
	SGUG_STEP_PENDING,
	SGUG_STEP_RUNNING,
	SGUG_STEP_DONE
} sgug_step_status;

typedef struct {
	const char *external_id;	/* the step's id */
	const char *name;
	int number;			/* 1 based */
	sgug_step_status status;
	const char *conclusion;		/* NULL unless done */
	const char *started_at;		/* may be NULL */
	const char *completed_at;	/* may be NULL */
} sgug_step_state;

/*
 * Pushes the current state of every step.
 *
 * This is what makes steps light up as they run rather than all appearing at
 * once when the job ends. change_order must increase across calls; the service
 * uses it to discard updates that arrive out of order.
 */
int sgug_results_steps_update(sgug_results *r, const sgug_step_state *steps,
    size_t nsteps, char *err, size_t errlen);

/*
 * Timestamp in the format the results service expects, which is three
 * fractional digits, not the seven the timeline API wants.
 */
void sgug_results_timestamp(sgug_time_t t, char *out, size_t outlen);

/*
 * Artifacts, on a second Twirp service at the same base:
 *
 *   twirp/github.actions.results.api.v1.ArtifactService/
 *
 * Upload is create, PUT the zip to the signed URL, finalize with the size and
 * a sha256. Download is get a signed URL and fetch it. The zip itself is built
 * and unpacked with the zip and unzip binaries, the same way checkout uses
 * git, since artifacts v4 are ordinary zip archives.
 */
int sgug_artifact_upload(sgug_results *r, const char *name,
    const char *zip_path, char *err, size_t errlen);

/* Writes the artifact zip to zip_path. */
int sgug_artifact_download(sgug_results *r, const char *name,
    const char *zip_path, char *err, size_t errlen);

#endif /* SGUG_PROTO_RESULTS_H */
