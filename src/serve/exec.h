#ifndef SGUG_SERVE_EXEC_H
#define SGUG_SERVE_EXEC_H

#include "net/http.h"
#include "proto/config.h"

#include <signal.h>
#include <stddef.h>

/*
 * Same signature as sgug_run_job, so either can sit behind sgug_job_runner.
 * http goes unused: the guest reports for itself, and the fallback completion
 * opens a connection of its own.
 *
 * The contract with the worker image:
 *
 *   /job            the staging directory, bind mounted from a host directory
 *                   the server creates 0700 and unlinks when the container
 *                   exits
 *   /job/job.json   the AgentJobRequestMessage as received, 0600
 *   /job/cancel     pre-created and empty. To cancel, the server changes its
 *                   mtime and then kills the container after a grace period
 *   /job/complete   created by the container once the guest's own completejob
 *                   has succeeded
 *   SGUG_RUNNER_NAME  the identity's agent name, for execjob --name
 *
 * job.json holds job->access_token, github_token and every isSecret variable,
 * so the staging directory is a credential store for the life of the job. The
 * container must not create subdirectories under /job; teardown unlinks one
 * level and removes the directory.
 *
 * cancel is pre-created because the guest reads it over NFS, where a file that
 * appears mid-job stays invisible for up to acdirmax, 60 seconds by default.
 *
 * The container's exit status is the emulator's, not the job's: iris has no
 * exec verb, so iris-ci scrapes a guest status off the serial console.
 * /job/complete alone decides whether the fallback completion is sent.
 */
int sgug_container_run_job(sgug_http_client *http, const sgug_config *cfg,
    const char *message, size_t message_len,
    volatile sig_atomic_t *stop, char *err, size_t errlen);

/* NULL image or 0 deadline leaves the built-in default. Call before forking;
 * children inherit these. */
void sgug_container_config(const char *image, int deadline_secs);

/* docker run --rm is the client's job, so a supervisor killed with SIGKILL
 * leaves its container running and the job credentials on disk. This removes
 * both, for every supervisor that is gone. */
void sgug_container_reap(void);

#endif /* SGUG_SERVE_EXEC_H */
