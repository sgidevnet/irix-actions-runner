#ifndef SGUG_EXEC_JOB_H
#define SGUG_EXEC_JOB_H

#include "compat/irix.h"
#include "json/json.h"

#include <stddef.h>

/*
 * The parsed shape of an AgentJobRequestMessage, reduced to what a runner that
 * executes `run:` steps actually needs.
 *
 * All strings point into the owning document, so the job must not outlive it.
 */

#define SGUG_JOB_MAX_STEPS 128

typedef enum {
	SGUG_STEP_SCRIPT,	/* run:, the only kind we execute */
	SGUG_STEP_ACTION,	/* uses:, dispatched to a native handler */
	SGUG_STEP_UNSUPPORTED	/* docker, container registry */
} sgug_step_kind;

typedef struct {
	const char *id;
	const char *display_name;
	const char *context_name;	/* the step's id:, or __run, __run_2 */
	const char *condition;		/* success(), always(), ... */
	sgug_step_kind kind;

	/* SGUG_STEP_SCRIPT */
	const char *script;
	const char *shell;		/* NULL means the default */
	const char *working_directory;	/* NULL means the workspace root */

	/* SGUG_STEP_ACTION */
	const char *action_name;	/* e.g. actions/checkout */
	const char *action_ref;		/* e.g. v4 */

	int continue_on_error;
	int timeout_minutes;		/* 0 when unset */
} sgug_step;

typedef struct {
	sgug_json_doc *doc;		/* owned; freed by sgug_job_free */

	const char *plan_id;
	const char *plan_type;		/* "actions"; the hub name in URLs */
	const char *timeline_id;
	const char *job_id;
	const char *job_name;
	const char *job_display_name;
	int64_t request_id;

	/*
	 * SystemVssConnection. The access token here authorises all job
	 * reporting and is not the runner's own OAuth token.
	 */
	const char *service_url;	/* endpoint url, the run service */
	const char *access_token;
	const char *pipelines_url;	/* base for timeline and log APIs */
	const char *results_url;

	sgug_step steps[SGUG_JOB_MAX_STEPS];
	size_t nsteps;

	/*
	 * Values the service wants kept out of logs. Both literal secrets and
	 * regexes arrive here; only the literals are usable without a regex
	 * engine, which is enough since the tokens that matter are literals.
	 */
	const char *masks[64];
	size_t nmasks;
} sgug_job;

/*
 * Parses a job message. Returns 0 on success. On failure returns -1 and writes
 * a reason to err.
 */
int sgug_job_parse(const char *text, size_t len, sgug_job *out,
    char *err, size_t errlen);

void sgug_job_free(sgug_job *job);

/*
 * Looks up a value in a context, e.g. sgug_job_context(job, "github",
 * "repository"). Returns fallback when absent.
 */
const char *sgug_job_context(const sgug_job *job, const char *context,
    const char *key, const char *fallback);

#endif /* SGUG_EXEC_JOB_H */
