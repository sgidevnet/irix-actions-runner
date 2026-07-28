#ifndef SGUG_EXEC_STEP_H
#define SGUG_EXEC_STEP_H

#include "exec/job.h"
#include "sandbox/confine.h"

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

/* Periodic heartbeat during a step. Must not block for long: it runs on the
 * thread draining the child's pipe. */
typedef void (*sgug_step_tick_fn)(void *ctx);

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

	/*
	 * Called periodically while the step runs, at most every POLL_SLICE_MS
	 * and also after each batch of output. Exists so the reporter can push
	 * console lines during a step instead of only when it ends; a step that
	 * prints nothing for a minute must still get its tail flushed.
	 */
	sgug_step_tick_fn tick_cb;
	void *tick_ctx;

	/* Applied in the child between fork and exec. NULL leaves the step
	 * unconfined, which is only appropriate for the runner's own helpers. */
	const sgug_confine_opts *confine;
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
 * Runs a program directly from an argv array, with no shell.
 *
 * Handlers use this rather than building a command line. A checkout puts an
 * installation token in a git config value, and passing that through a shell
 * would mean quoting a secret correctly every time, forever. argv has no
 * quoting to get wrong.
 *
 * argv[0] is the program, found on the opts PATH if it has no slash. Returns
 * the exit status, or -1 if it could not be started.
 */
int sgug_run_argv(const char *const *argv, const char *cwd,
    const sgug_step_opts *opts, sgug_step_output_fn on_line, void *ctx,
    char *err, size_t errlen);

#endif /* SGUG_EXEC_STEP_H */
