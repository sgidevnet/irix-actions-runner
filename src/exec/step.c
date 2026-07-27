#include "exec/step.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define READ_CHUNK 4096
#define LINE_MAX_LEN 8192
#define POLL_SLICE_MS 500

static void
seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;

	va_start(ap, fmt);
	sgug_vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

int
sgug_step_should_run(const char *condition, int job_failed, int job_cancelled)
{
	if (condition == NULL || *condition == '\0')
		return !job_failed && !job_cancelled;

	if (strstr(condition, "always()") != NULL)
		return 1;
	if (strstr(condition, "cancelled()") != NULL ||
	    strstr(condition, "canceled()") != NULL)
		return job_cancelled;
	if (strstr(condition, "failure()") != NULL)
		return job_failed && !job_cancelled;

	/* success(), and anything we do not parse. */
	return !job_failed && !job_cancelled;
}

/* Picks the interpreter. SGUG's bash first, since workflows assume bash. */
static const char *
resolve_shell(const char *want)
{
	static const char *const CANDIDATES[] = {
		"/usr/sgug/bin/bash", "/bin/bash", "/sbin/sh", "/bin/sh"
	};
	size_t i;

	if (want != NULL && *want != '\0') {
		if (strchr(want, '/') != NULL)
			return want;
		if (strcmp(want, "bash") == 0) {
			if (access("/usr/sgug/bin/bash", X_OK) == 0)
				return "/usr/sgug/bin/bash";
			if (access("/bin/bash", X_OK) == 0)
				return "/bin/bash";
		}
		/* sh, or an unknown name: fall through to the defaults rather
		 * than fail, since /bin/sh runs most workflow scripts. */
	}

	for (i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
		if (access(CANDIDATES[i], X_OK) == 0)
			return CANDIDATES[i];
	}
	return "/bin/sh";
}

static int
write_script(const char *dir, const char *script, char *path, size_t pathlen,
    char *err, size_t errlen)
{
	int fd;
	size_t n = strlen(script);
	ssize_t w;

	sgug_snprintf(path, pathlen, "%s/step-%ld.sh", dir, (long)getpid());

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
	if (fd < 0) {
		seterr(err, errlen, "cannot create %s: %s", path,
		    strerror(errno));
		return -1;
	}

	w = write(fd, script, n);
	if (close(fd) != 0 || w < 0 || (size_t)w != n) {
		seterr(err, errlen, "cannot write %s", path);
		unlink(path);
		return -1;
	}
	return 0;
}

/* Emits complete lines from buf, keeping any partial tail for the next read. */
static size_t
drain_lines(char *buf, size_t len, sgug_step_output_fn on_line, void *ctx)
{
	size_t start = 0, i;

	for (i = 0; i < len; i++) {
		if (buf[i] != '\n')
			continue;

		buf[i] = '\0';
		/* Tolerate CRLF from anything that writes DOS line endings. */
		if (i > start && buf[i - 1] == '\r')
			buf[i - 1] = '\0';
		if (on_line != NULL)
			on_line(ctx, buf + start);
		start = i + 1;
	}

	if (start > 0 && start < len)
		memmove(buf, buf + start, len - start);
	return start >= len ? 0 : len - start;
}

int
sgug_step_run(const sgug_step *step, const sgug_step_opts *opts,
    sgug_step_output_fn on_line, void *ctx, char *err, size_t errlen)
{
	char script_path[512];
	char buf[LINE_MAX_LEN];
	const char *shell;
	int pipefd[2];
	pid_t pid;
	size_t held = 0;
	int64_t deadline;
	int status = 0, killed = 0, rc = -1;

	if (step->script == NULL || *step->script == '\0') {
		/* An empty script is a no-op, not a failure. */
		return 0;
	}

	if (write_script(opts->temp_dir, step->script, script_path,
	    sizeof(script_path), err, errlen) != 0)
		return -1;

	shell = resolve_shell(step->shell != NULL ? step->shell : opts->shell);

	if (pipe(pipefd) != 0) {
		seterr(err, errlen, "pipe: %s", strerror(errno));
		unlink(script_path);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		seterr(err, errlen, "fork: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		unlink(script_path);
		return -1;
	}

	if (pid == 0) {
		char *argv[3];
		const char *dir;

		close(pipefd[0]);

		/* Both streams go to the caller interleaved, which is what the
		 * Actions log shows. */
		if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
		    dup2(pipefd[1], STDERR_FILENO) < 0)
			_exit(126);
		close(pipefd[1]);

		/* No stdin. A step that reads it would otherwise block
		 * forever on an inherited terminal. */
		{
			int devnull = open("/dev/null", O_RDONLY);

			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				close(devnull);
			}
		}

		dir = step->working_directory != NULL &&
		    *step->working_directory != '\0'
		    ? step->working_directory : opts->work_dir;
		if (dir != NULL && chdir(dir) != 0) {
			/* Relative to the workspace, per the workflow schema. */
			if (chdir(opts->work_dir) != 0 ||
			    (step->working_directory != NULL &&
			    chdir(step->working_directory) != 0))
				_exit(127);
		}

		argv[0] = (char *)shell;
		argv[1] = script_path;
		argv[2] = NULL;

		if (opts->env != NULL)
			execve(shell, argv, (char *const *)opts->env);
		else
			execv(shell, argv);
		_exit(127);
	}

	close(pipefd[1]);

	deadline = opts->timeout_seconds > 0
	    ? sgug_monotonic_ms() + (int64_t)opts->timeout_seconds * 1000
	    : 0;

	for (;;) {
		struct pollfd pfd;
		int pr;
		ssize_t n;

		pfd.fd = pipefd[0];
		pfd.events = POLLIN;
		pfd.revents = 0;

		pr = poll(&pfd, 1, POLL_SLICE_MS);

		if (pr < 0 && errno != EINTR) {
			seterr(err, errlen, "poll: %s", strerror(errno));
			break;
		}

		if (pr > 0) {
			n = read(pipefd[0], buf + held, sizeof(buf) - held - 1);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;	/* child closed the pipe */

			held += (size_t)n;
			held = drain_lines(buf, held, on_line, ctx);

			/*
			 * A line longer than the buffer is emitted in pieces
			 * rather than growing without bound, which a runaway
			 * step could otherwise use to exhaust 2 GB.
			 */
			if (held >= sizeof(buf) - 1) {
				buf[held] = '\0';
				if (on_line != NULL)
					on_line(ctx, buf);
				held = 0;
			}
			continue;
		}

		/* Timed out or interrupted: check whether to give up. */
		if (!killed && opts->abort_cb != NULL &&
		    opts->abort_cb(opts->abort_ctx) != 0) {
			kill(pid, SIGTERM);
			killed = 1;
			continue;
		}
		if (!killed && deadline != 0 && sgug_monotonic_ms() >= deadline) {
			if (on_line != NULL)
				on_line(ctx, "step timed out");
			kill(pid, SIGTERM);
			killed = 1;
			continue;
		}
	}

	/* Anything left without a trailing newline is still a line. */
	if (held > 0) {
		buf[held] = '\0';
		if (on_line != NULL)
			on_line(ctx, buf);
	}

	close(pipefd[0]);

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			break;
	}

	unlink(script_path);

	if (killed)
		return SGUG_STEP_ABORTED;

	if (WIFEXITED(status))
		rc = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		rc = 128 + WTERMSIG(status);
	else
		rc = 1;

	return rc;
}
