#ifndef SGUG_EXEC_HANDLERS_H
#define SGUG_EXEC_HANDLERS_H

#include "exec/job.h"
#include "exec/step.h"

/*
 * Native implementations of the few `uses:` actions worth having.
 *
 * There is no JavaScript engine here, so an action either has a handler or it
 * fails. That is a smaller loss than it sounds: the actions an IRIX CI needs
 * are a short list, and each is a few hundred lines around a tool the machine
 * already has.
 *
 * Matching ignores the version, so actions/checkout@v3 and @v4 both resolve
 * here. The inputs that differ between versions are ones we do not implement
 * anyway.
 */

typedef int (*sgug_action_fn)(const sgug_job *job, const sgug_step *step,
    const sgug_step_opts *opts, sgug_step_output_fn on_line, void *ctx,
    char *err, size_t errlen);

/* NULL when nothing handles this action. */
sgug_action_fn sgug_action_lookup(const char *action_name);

/* Comma-separated list of what is supported, for error messages. */
const char *sgug_action_supported(void);

#endif /* SGUG_EXEC_HANDLERS_H */
