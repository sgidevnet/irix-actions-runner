/*
 * Container executor: one ephemeral worker container per job, supervised by
 * the identity's own listener process. Linux host only; see serve/exec.h for
 * the contract with the image.
 */

#include "compat/irix.h"
#include "exec/job.h"
#include "proto/report.h"
#include "serve/dockerapi.h"
#include "serve/exec.h"

#include "version.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_SECS 2

/* A daemon reload answers nothing for a few seconds. Consecutive misses. */
#define INSPECT_MAX_MISSES 5

/* The guest must see the cancel mtime across an NFS attribute cache first. */
#define CANCEL_GRACE_SECS 120

/* Nothing else bounds a hung job: step timeout-minutes is parsed and never
 * read, and JobCancellation cannot reach a wedged emulator. */
#define DEFAULT_DEADLINE_SECS 14400

/* A kill that keeps failing while inspect keeps answering would otherwise
 * hold the identity at currentParallelism 1 forever. */
#define KILL_MAX_TRIES 5

/* A staging directory path plus one of the names the contract puts under it. */
#define STAGING_PATH_MAX 600

/* One pass reaps this many; the next runs REAP_INTERVAL_SECS later. */
#define REAP_MAX 64

#define LABEL_SUPERVISOR "sgug.runner.supervisor"
#define LABEL_STAGING "sgug.runner.staging"

static char worker_image[256] = "irix-worker:latest";
static int deadline_secs = DEFAULT_DEADLINE_SECS;

void
sgug_container_config(const char *image, int secs)
{
	if (image != NULL && *image != '\0')
		sgug_snprintf(worker_image, sizeof(worker_image), "%s", image);
	if (secs > 0)
		deadline_secs = secs;
}

static int
remove_container(const char *cid)
{
	char err[256];

	if (sgug_docker_remove(cid, err, sizeof(err)) != 0) {
		fprintf(stderr, "remove %.12s: %s\n", cid, err);
		return -1;
	}
	return 0;
}

static void
remove_staging(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[STAGING_PATH_MAX];

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		sgug_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	rmdir(dir);
}

static int
write_file(const char *path, const char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	size_t off = 0;

	if (fd < 0)
		return -1;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)n;
	}
	return close(fd);
}

static int
make_staging(char *out, size_t outlen, const char *message, size_t len,
    char *err, size_t errlen)
{
	char path[STAGING_PATH_MAX];

	sgug_snprintf(out, outlen, "%s/sgug-job-XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	/* mkdtemp creates it 0700, which is what keeps the job token out of
	 * reach of every other user on the host. */
	if (mkdtemp(out) == NULL) {
		sgug_snprintf(err, errlen, "staging directory: %s",
		    strerror(errno));
		return -1;
	}

	sgug_snprintf(path, sizeof(path), "%s/job.json", out);
	if (write_file(path, message, len) != 0) {
		sgug_snprintf(err, errlen, "%s: %s", path, strerror(errno));
		remove_staging(out);
		return -1;
	}

	sgug_snprintf(path, sizeof(path), "%s/cancel", out);
	if (write_file(path, "", 0) != 0) {
		sgug_snprintf(err, errlen, "%s: %s", path, strerror(errno));
		remove_staging(out);
		return -1;
	}
	return 0;
}

static int
start_container(const sgug_config *cfg, const char *staging, char *cid,
    size_t cidlen, char *err, size_t errlen)
{
	sgug_docker_label labels[2];
	const char *env[1];
	char bind[640], supervisor[24], name[SGUG_MAX_NAME + 32];

	/*
	 * :Z relabels the staging directory for this container alone. Without
	 * it every read inside fails EACCES on an SELinux host. Docker ignores
	 * the suffix where SELinux is off.
	 */
	sgug_snprintf(bind, sizeof(bind), "%s:/job:Z", staging);
	sgug_snprintf(supervisor, sizeof(supervisor), "%ld", (long)getpid());
	sgug_snprintf(name, sizeof(name), "SGUG_RUNNER_NAME=%s",
	    cfg->agent_name);

	env[0] = name;
	labels[0].key = LABEL_SUPERVISOR;
	labels[0].value = supervisor;
	labels[1].key = LABEL_STAGING;
	labels[1].value = staging;

	return sgug_docker_run(worker_image, bind, env, 1, labels, 2, cid,
	    cidlen, err, errlen);
}

static void
touch_cancel(const char *staging)
{
	char path[STAGING_PATH_MAX];

	sgug_snprintf(path, sizeof(path), "%s/cancel", staging);
	utimes(path, NULL);
}

int64_t
sgug_container_monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
supervise(const char *cid, const char *staging, volatile sig_atomic_t *stop,
    int *cancelled, int *timed_out)
{
	int64_t started = sgug_container_monotonic_ms();
	int64_t cancelled_at = 0;
	char err[256];
	int killed = 0, misses = 0, kill_fails = 0;

	for (;;) {
		int64_t now;
		int running, code, expired, unresponsive;

		if (sgug_docker_inspect(cid, &running, &code, err,
		    sizeof(err)) != 0) {
			/* Giving up while it still runs would report the job
			 * from here and let the guest report it again. */
			if (++misses > INSPECT_MAX_MISSES) {
				fprintf(stderr, "inspect %.12s: %s\n", cid,
				    err);
				sgug_docker_kill(cid, err, sizeof(err));
				return -1;
			}
			sleep(POLL_SECS);
			continue;
		}
		misses = 0;
		if (!running)
			return code;

		now = sgug_container_monotonic_ms();
		if (stop != NULL && *stop && !*cancelled) {
			*cancelled = 1;
			cancelled_at = now;
			touch_cancel(staging);
		}

		expired = now - started > (int64_t)deadline_secs * 1000;
		unresponsive = *cancelled && now - cancelled_at >
		    (int64_t)CANCEL_GRACE_SECS * 1000;

		if (!killed && (expired || unresponsive)) {
			*timed_out = expired;
			if (sgug_docker_kill(cid, err, sizeof(err)) == 0) {
				killed = 1;
			} else if (++kill_fails > KILL_MAX_TRIES) {
				fprintf(stderr, "kill %.12s: %s\n", cid, err);
				return -1;
			}
		}
		sleep(POLL_SECS);
	}
}

/* Gates the fallback. A double completion is benign where it slips through:
 * the second completejob is answered 404 and the run keeps its conclusion. */
static int
guest_completed(const char *staging)
{
	char path[STAGING_PATH_MAX];
	struct stat st;

	sgug_snprintf(path, sizeof(path), "%s/complete", staging);
	return stat(path, &st) == 0;
}

/*
 * Nothing renews a job lock: requestId is 0 and lockedUntil is
 * DateTime.MinValue on v2, so an unreported job leaves that identity online,
 * at currentParallelism 1 and never dispatched to again. docs/protocol.md.
 *
 * Its own client, not the caller's: that one aborts every read on the
 * listener's stop flag, which is already set whenever this is reached with a
 * cancellation.
 */
static int
fallback_complete(const sgug_job *job, sgug_result result, char *err,
    size_t errlen)
{
	sgug_http_client *http = sgug_http_client_new(NULL, SGUG_USER_AGENT);
	sgug_reporter *rep;
	int rc;

	if (http == NULL) {
		sgug_snprintf(err, errlen, "out of memory");
		return -1;
	}

	rep = sgug_report_new(http, job);
	if (rep == NULL) {
		sgug_http_client_free(http);
		sgug_snprintf(err, errlen, "out of memory");
		return -1;
	}

	rc = sgug_report_job_finished(rep, result);
	if (rc != 0)
		sgug_snprintf(err, errlen, "fallback completion: %s",
		    sgug_report_last_error(rep));

	sgug_report_free(rep);
	sgug_http_client_free(http);
	return rc;
}

int
sgug_container_run_job(sgug_http_client *http, const sgug_config *cfg,
    const char *message, size_t message_len,
    volatile sig_atomic_t *stop, char *err, size_t errlen)
{
	sgug_job job;
	char staging[512];
	char cid[80];
	char ferr[256];
	int cancelled = 0, timed_out = 0, completed = 0;
	int status, ran = 0, rc = -1;

	if (sgug_job_parse(message, message_len, &job, err, errlen) != 0)
		return -1;

	if (make_staging(staging, sizeof(staging), message, message_len, err,
	    errlen) != 0)
		goto out;

	if (start_container(cfg, staging, cid, sizeof(cid), err, errlen) != 0)
		goto out;

	printf("job       %s in %.12s\n", job.job_display_name, cid);
	fflush(stdout);

	status = supervise(cid, staging, stop, &cancelled, &timed_out);
	ran = 1;
	rc = 0;
	completed = guest_completed(staging);
	if (!completed)
		fprintf(stderr, "container %.12s exited %d%s without a "
		    "completion; reporting the job from here\n", cid, status,
		    timed_out ? " on the deadline" : "");

	/*
	 * Every path past the parse unwinds through here: the slot was taken
	 * at acquire, so a daemon that is down or an image that is missing has
	 * to be reported from here just as a dead container does.
	 */
out:
	if (!completed && fallback_complete(&job,
	    cancelled ? SGUG_RESULT_CANCELED : SGUG_RESULT_FAILED,
	    ferr, sizeof(ferr)) != 0) {
		/* err already names the failure that got us here. */
		if (rc == 0)
			sgug_snprintf(err, errlen, "%s", ferr);
		else
			fprintf(stderr, "%s\n", ferr);
		rc = -1;
	}

	/* A container whose removal failed still has the staging directory
	 * bind mounted, so leave the pair for the reaper. */
	if (!ran || remove_container(cid) == 0)
		remove_staging(staging);

	sgug_job_free(&job);
	return rc;
}

void
sgug_container_reap(void)
{
	sgug_docker_container list[REAP_MAX];
	char err[256];
	size_t n, i;
	int truncated;

	/* truncated goes unread: the next pass takes what this one left. */
	if (sgug_docker_list(LABEL_SUPERVISOR, LABEL_STAGING, list, REAP_MAX,
	    &n, &truncated, err, sizeof(err)) != 0)
		return;

	for (i = 0; i < n; i++) {
		/* Anything but ESRCH means the process is there, EPERM
		 * included. A reused pid reads as alive and the container is
		 * left running, which is the direction to be wrong in. */
		if (kill((pid_t)strtol(list[i].supervisor, NULL, 10), 0) == 0 ||
		    errno != ESRCH)
			continue;

		fprintf(stderr, "reaping %.12s, supervisor %s is gone\n",
		    list[i].id, list[i].supervisor);
		if (remove_container(list[i].id) == 0 &&
		    list[i].staging[0] == '/')
			remove_staging(list[i].staging);
	}
}
