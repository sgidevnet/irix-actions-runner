#ifndef SGUG_EXEC_STEP_H
#define SGUG_EXEC_STEP_H

#include "exec/job.h"

#include <stddef.h>

/*
 * Runs one `run:` step.
 *
 * The script goes to a file and the shell is exec'd on it rather than passed
 * -c: a workflow script is arbitrary multi-line shell, routinely containing
 * quotes and newlines, and IRIX has no posix_spawn to hide the difference.
 */

/* Called once per line of combined stdout and stderr, without the newline. */
typedef void (*sgug_step_output_fn)(void *ctx, const char *line);

/* Consulted between reads; return non-zero to kill the step. */
typedef int (*sgug_step_abort_fn)(void *ctx);

typedef struct {
	const char *work_dir;		/* job workspace, the step's cwd */
	const char *temp_dir;		/* scratch for the script file */
	const char *shell;		/* NULL selects bash then sh */
	int timeout_seconds;		/* 0 means no limit */

	/*
	 * Environment given to the step, as "NAME=value" strings. Built by the
	 * caller rather than inherited: the runner's own environment holds its
	 * credentials, and the user's profile can carry settings a job should
	 * not see, such as a global http.sslverify=false.
	 */
	char *const *env;
	size_t nenv;

	sgug_step_abort_fn abort_cb;
	void *abort_ctx;
} sgug_step_opts;

/*
 * Returns the exit status, or -1 if the step could not be started, with a
 * reason in err. A step killed by a signal reports 128 plus the signal, as a
 * shell would. A step stopped by the timeout or by abort_cb reports -2.
 */
#define SGUG_STEP_ABORTED (-2)

int sgug_step_run(const sgug_step *step, const sgug_step_opts *opts,
    sgug_step_output_fn on_line, void *ctx, char *err, size_t errlen);

/*
 * Evaluates a step condition against the job state so far.
 *
 * Only the four forms the service actually emits for ordinary workflows are
 * understood: success(), always(), failure(), cancelled(). Anything else is
 * treated as success(), which is the permissive choice: running a step that
 * should have been skipped is visible in the log, whereas silently skipping
 * one the author expected to run is not.
 */
int sgug_step_should_run(const char *condition, int job_failed, int job_cancelled);

#endif /* SGUG_EXEC_STEP_H */
