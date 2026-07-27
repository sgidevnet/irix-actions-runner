#ifndef SGUG_PROTO_REPORT_H
#define SGUG_PROTO_REPORT_H

#include "exec/job.h"
#include "net/http.h"

#include <stddef.h>

/*
 * Job reporting: timeline records, logs, live console output, completion.
 *
 * Every call here authenticates with the job's own access token from
 * SystemVssConnection, not the runner's OAuth token, and addresses
 * PipelinesServiceUrl rather than the tenant URL. Using the runner token gets
 * a 401 that looks like the runner is unregistered.
 */

typedef enum {
	SGUG_RESULT_SUCCEEDED,
	SGUG_RESULT_FAILED,
	SGUG_RESULT_CANCELED,
	SGUG_RESULT_SKIPPED
} sgug_result;

typedef struct sgug_reporter sgug_reporter;

/* Borrows job, which must outlive the reporter. */
sgug_reporter *sgug_report_new(sgug_http_client *http, const sgug_job *job);
void sgug_report_free(sgug_reporter *r);

/*
 * Creates the timeline records: one Job record for the job and one Task record
 * per step, all Pending. The service pre-creates the job record, so this is a
 * merge rather than an insert.
 */
int sgug_report_begin(sgug_reporter *r);

/* Marks the job in progress. */
int sgug_report_job_started(sgug_reporter *r);

/* Marks step i in progress. */
int sgug_report_step_started(sgug_reporter *r, size_t i);

/*
 * Appends console output for step i. Lines are batched and flushed by
 * sgug_report_flush; secrets named in the job's mask list are replaced before
 * anything leaves the machine.
 */
int sgug_report_step_output(sgug_reporter *r, size_t i, const char *line);

/* Sends any buffered console lines. Safe to call often. */
int sgug_report_flush(sgug_reporter *r);

/*
 * Completes step i: uploads its captured log, links it to the record, and sets
 * the final state.
 */
int sgug_report_step_finished(sgug_reporter *r, size_t i, sgug_result result);

/*
 * Completes the job. Sends the final timeline state and then the JobCompleted
 * plan event, which is what releases the runner's parallelism slot. A runner
 * that skips this leaves the agent permanently undispatchable.
 */
int sgug_report_job_finished(sgug_reporter *r, sgug_result result);

const char *sgug_report_last_error(const sgug_reporter *r);

#endif /* SGUG_PROTO_REPORT_H */
