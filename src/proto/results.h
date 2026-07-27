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
 * Uploads one step's complete log and registers it. line_count is what the UI
 * shows as the log length.
 */
int sgug_results_step_log(sgug_results *r, const char *step_id,
    const char *log, size_t len, int64_t line_count, char *err, size_t errlen);

/*
 * Uploads the whole-job log. Separate from the per-step logs and not derived
 * from them: the steps drive what each section shows when expanded, while this
 * is what the download-log button and the REST logs endpoint serve. Without it
 * that endpoint answers BlobNotFound even though the steps look complete.
 */
int sgug_results_job_log(sgug_results *r, const char *log, size_t len,
    int64_t line_count, char *err, size_t errlen);

#endif /* SGUG_PROTO_RESULTS_H */
