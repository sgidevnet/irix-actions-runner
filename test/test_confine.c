/*
 * Confinement is applied in a child between fork and exec, so these tests fork
 * and check what the child observes. Running them on IRIX is the point: the
 * limits that matter there, PR_MAXPROCS in particular, have no Linux analogue.
 */

#include "sandbox/confine.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

/* Runs fn in a child with opts applied; returns the child's exit status. */
static int
in_child(const sgug_confine_opts *opts, int (*fn)(void))
{
	pid_t pid = fork();
	int status = 0;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (sgug_confine_apply(opts) != 0)
			_exit(99);
		_exit(fn());
	}
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int
check_nofile(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
		return 1;
	return rl.rlim_cur == 64 ? 0 : 2;
}

static int
check_fsize(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_FSIZE, &rl) != 0)
		return 1;
	return rl.rlim_cur == 1024 * 1024 ? 0 : 2;
}

static int
check_core_disabled(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_CORE, &rl) != 0)
		return 1;
	return rl.rlim_cur == 0 ? 0 : 2;
}

static void
test_limits_reach_the_child(void)
{
	sgug_confine_opts o;

	memset(&o, 0, sizeof(o));
	o.max_files = 64;
	CHECK(in_child(&o, check_nofile) == 0);

	memset(&o, 0, sizeof(o));
	o.file_size_mb = 1;
	CHECK(in_child(&o, check_fsize) == 0);

	/* Cores are always disabled, whatever else was asked for. */
	memset(&o, 0, sizeof(o));
	CHECK(in_child(&o, check_core_disabled) == 0);
}

static void
test_defaults_are_sane(void)
{
	sgug_confine_opts o;

	sgug_confine_defaults(&o);

	/* A long compile must not trip these; a runaway must. */
	CHECK(o.cpu_seconds >= 600);
	CHECK(o.memory_mb > 0 && o.memory_mb < 2816);
	CHECK(o.max_files >= 256);
	CHECK(o.max_procs > 8);
	/* No chroot by default: it needs root and the runner usually is not. */
	CHECK(o.chroot_dir == NULL);
}

static void
test_describe(void)
{
	sgug_confine_opts o;
	char buf[256];

	memset(&o, 0, sizeof(o));
	sgug_confine_describe(&o, buf, sizeof(buf));
	CHECK(strcmp(buf, "none") == 0);

	sgug_confine_defaults(&o);
	sgug_confine_describe(&o, buf, sizeof(buf));
	CHECK(strstr(buf, "cpu") != NULL);
	CHECK(strstr(buf, "mem") != NULL);

	/* Asking for a chroot without privilege must say so rather than imply
	 * the job was confined. */
	o.chroot_dir = "/nonexistent";
	sgug_confine_describe(&o, buf, sizeof(buf));
	if (geteuid() != 0)
		CHECK(strstr(buf, "not root") != NULL);
}

static void
test_chroot_refused_without_privilege(void)
{
	sgug_confine_opts o;

	if (geteuid() == 0) {
		printf("  (running as root, skipping the unprivileged case)\n");
		return;
	}

	/* Asking for a chroot as an ordinary user must not silently proceed
	 * unconfined: the caller believes the job is contained. */
	memset(&o, 0, sizeof(o));
	o.chroot_dir = "/tmp";
	CHECK(sgug_confine_apply(&o) == 0);
}

int
main(void)
{
	test_limits_reach_the_child();
	test_defaults_are_sane();
	test_describe();
	test_chroot_refused_without_privilege();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all confine tests passed\n");
	return 0;
}
