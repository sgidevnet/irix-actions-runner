#ifndef SGUG_SERVE_SERVE_H
#define SGUG_SERVE_SERVE_H

#include "net/http.h"
#include "proto/config.h"

#include <signal.h>
#include <stddef.h>
#include <stdio.h>

/* configure --count is capped here: serve's child array is fixed at this. */
#define SGUG_MAX_IDENTITIES 64

typedef struct {
	const char *name_prefix;	/* NULL runs one identity in "." */
	const char *image;		/* NULL keeps the built-in default */
	int count;
	int job_timeout;		/* seconds; 0 keeps the default */
	int verbose;
} sgug_serve_opts;

/* What the listener calls for a job: sgug_run_job locally,
 * sgug_container_run_job under serve. */
typedef int (*sgug_job_runner)(sgug_http_client *http, const sgug_config *cfg,
    const char *message, size_t message_len, volatile sig_atomic_t *stop,
    char *err, size_t errlen);

/* One configured directory's listener loop, run to the end of its life. */
typedef int (*sgug_listen_dir)(const char *dir, int verbose,
    sgug_job_runner run);

#if defined(__sgi)

/* src/serve/ is not built on IRIX. Refusing here rather than in main.c keeps
 * the platform conditional out of the command table. */
#define sgug_serve(opts, fn) \
	((void)(opts), (void)(fn), \
	    fputs("serve drives the container executor and runs on a Linux " \
	    "host, not on IRIX\n", stderr), 2)

#else

/*
 * Forks one child per configured identity, returning when all have exited.
 *
 * The fork comes before anything touches the network: sgug_http_client caches
 * one keep-alive TLS connection per host and port, and children sharing it
 * interleave their bytes into one TLS stream. The symptom is a record MAC
 * failure, which points nowhere.
 *
 * fn() must unblock SIGINT and SIGTERM once it has installed its own handler;
 * both are blocked across the fork.
 */
int sgug_serve(const sgug_serve_opts *opts, sgug_listen_dir fn);

#endif /* __sgi */

#endif /* SGUG_SERVE_SERVE_H */
