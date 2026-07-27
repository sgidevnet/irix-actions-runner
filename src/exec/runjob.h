#ifndef SGUG_EXEC_RUNJOB_H
#define SGUG_EXEC_RUNJOB_H

#include "net/http.h"
#include "proto/config.h"

#include <signal.h>
#include <stddef.h>

/*
 * Executes one job end to end: parse, prepare the workspace, run each step
 * while streaming output, and report the result.
 *
 * Returns 0 when the job was reported, whatever its result, and -1 only when
 * the job could not be reported at all. Completion is always attempted: the
 * JobCompleted event is what releases this runner's parallelism slot, and a
 * runner that skips it becomes permanently undispatchable.
 */
int sgug_run_job(sgug_http_client *http, const sgug_config *cfg,
    const char *message, size_t message_len,
    volatile sig_atomic_t *stop, char *err, size_t errlen);

#endif /* SGUG_EXEC_RUNJOB_H */
