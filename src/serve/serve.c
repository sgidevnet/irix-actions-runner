/*
 * `runner serve`: one listener process per configured identity, each running
 * jobs in its own worker container. Linux host only.
 */

#include "compat/irix.h"
#include "proto/config.h"
#include "serve/dockerapi.h"
#include "serve/exec.h"
#include "serve/serve.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REAP_INTERVAL_SECS 30

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

struct child {
	pid_t pid;
	char dir[SGUG_MAX_NAME];
};

/*
 * serve does not touch the daemon until a job has been acquired, so without
 * this the socket is found out about one dispatched and failed job at a time.
 */
int
sgug_serve_selftest(void)
{
	char version[64], err[256];

	if (sgug_docker_ping(version, sizeof(version), err, sizeof(err)) != 0) {
		printf("docker    %s\n", err);
		printf("          serve runs each job in a container and\n");
		printf("          needs this socket; `run` does not\n");
		return -1;
	}
	printf("docker    engine %s\n", version);
	return 0;
}

int
sgug_serve(const sgug_serve_opts *opts, sgug_listen_dir fn)
{
	struct child kids[SGUG_MAX_IDENTITIES];
	struct sigaction sa;
	sigset_t block;
	int64_t reaped;
	int i, n, nkids, live, forwarded = 0, rc = 0;

	nkids = opts->count > 0 ? opts->count : 1;
	for (n = 0; n < nkids; n++) {
		if (opts->name_prefix != NULL)
			sgug_snprintf(kids[n].dir, sizeof(kids[n].dir),
			    "%s-%d", opts->name_prefix, n);
		else
			sgug_snprintf(kids[n].dir, sizeof(kids[n].dir), ".");
		kids[n].pid = 0;
		if (!sgug_config_exists(kids[n].dir)) {
			fprintf(stderr, "no runner configured in %s; run "
			    "`runner configure` first\n", kids[n].dir);
			return 1;
		}
	}

	sgug_container_config(opts->image, opts->job_timeout);

	/* Without SA_RESTART, so the sleep below returns as soon as the signal
	 * lands rather than a second later. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/*
	 * A child inherits this handler, which sets a flag nothing in the child
	 * reads, so a signal landing before fn() installs its own is lost.
	 * Forwarding again on every pass of the wait loop would cover the
	 * window too, but the repeats land during the child's session delete,
	 * where SSL_connect fails outright on EINTR.
	 */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigprocmask(SIG_BLOCK, &block, NULL);

	for (n = 0; n < nkids; n++) {
		fflush(stdout);
		kids[n].pid = fork();
		if (kids[n].pid < 0) {
			fprintf(stderr, "fork: %s\n", strerror(errno));
			stop_requested = 1;
			nkids = n;
			rc = 1;
			break;
		}
		if (kids[n].pid == 0)
			exit(fn(kids[n].dir, opts->verbose,
			    sgug_container_run_job));
		printf("serve     %s pid %ld\n", kids[n].dir,
		    (long)kids[n].pid);
	}
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	fflush(stdout);

	/* Zero, not now: a serve killed by docker stop before its own grace
	 * elapsed leaves containers and their job tokens behind, and this is
	 * the only thing that collects them. */
	reaped = 0;

	/* A child deletes its pool session on the way out; a parent that exits
	 * first leaves N orphans, each a minute of 409 retries next start. */
	for (live = nkids; live > 0; ) {
		pid_t pid;
		int st;

		/* Children got their own copy of the flag at fork, and docker
		 * stop signals PID 1 only. */
		if (stop_requested && !forwarded) {
			for (i = 0; i < nkids; i++)
				if (kids[i].pid > 0)
					kill(kids[i].pid, SIGTERM);
			forwarded = 1;
		}

		pid = waitpid(-1, &st, WNOHANG);
		if (pid < 0 && errno != EINTR)
			break;
		if (pid > 0) {
			for (i = 0; i < nkids; i++) {
				if (kids[i].pid != pid)
					continue;
				kids[i].pid = 0;
				live--;
				if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
					break;
				rc = 1;
				fprintf(stderr, "%s stopped: %s %d\n",
				    kids[i].dir,
				    WIFSIGNALED(st) ? "signal" : "status",
				    WIFSIGNALED(st) ? WTERMSIG(st) :
				    WEXITSTATUS(st));
				break;
			}
			continue;
		}

		if (sgug_container_monotonic_ms() - reaped >=
		    (int64_t)REAP_INTERVAL_SECS * 1000) {
			sgug_container_reap();
			reaped = sgug_container_monotonic_ms();
		}
		sleep(1);
	}

	return rc;
}
