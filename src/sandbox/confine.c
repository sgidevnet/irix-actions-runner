#include "sandbox/confine.h"

#include "compat/irix.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__sgi)
#include <sys/prctl.h>
#endif

void
sgug_confine_defaults(sgug_confine_opts *opts)
{
	memset(opts, 0, sizeof(*opts));

	/*
	 * An hour of CPU, not wall clock. A long compile is legitimate; an
	 * infinite loop is not, and the two look identical until this fires.
	 */
	opts->cpu_seconds = 3600;

	/*
	 * Under 2816 MB of RAM, leaving room for the runner, the OS and a
	 * little swap headroom. A link step that allocates past this fails
	 * with a clear ENOMEM rather than driving the machine into swap, where
	 * a 400MHz R12000 becomes unusable for everything else.
	 */
	opts->memory_mb = 1536;

	/* 35 GB is free, so this is only a guard against a runaway writer. */
	opts->file_size_mb = 4096;

	opts->max_files = 512;

	/* The fork bomb cap. Enough for make -j2 and its toolchain, far short
	 * of enough to fill the process table. */
	opts->max_procs = 96;
}

static int
set_limit(int resource, rlim_t value)
{
	struct rlimit rl;

	if (getrlimit(resource, &rl) != 0)
		return -1;

	/* Never raise a ceiling an operator lowered, and never exceed the hard
	 * limit, which an unprivileged process cannot do anyway. */
	if (rl.rlim_max != RLIM_INFINITY && value > rl.rlim_max)
		value = rl.rlim_max;

	rl.rlim_cur = value;
	return setrlimit(resource, &rl) == 0 ? 0 : -1;
}

int
sgug_confine_apply(const sgug_confine_opts *opts)
{
	int rc = 0;

	/*
	 * Privileged layers first, because chroot must happen before the uid
	 * drop that makes it irreversible.
	 */
	if (opts->chroot_dir != NULL && *opts->chroot_dir != '\0' &&
	    geteuid() == 0) {
		if (chroot(opts->chroot_dir) != 0 || chdir("/") != 0)
			return -1;

		/* setgroups before setgid before setuid. Any other order leaves
		 * the process holding privilege it meant to give up. */
		if (setgroups(0, NULL) != 0)
			return -1;
		if (setgid(opts->gid) != 0 || setuid(opts->uid) != 0)
			return -1;

		/* Belt and braces: if the uid drop silently failed we must not
		 * continue into the job. */
		if (geteuid() == 0 || getuid() == 0)
			return -1;
	}

	if (opts->cpu_seconds > 0 &&
	    set_limit(RLIMIT_CPU, (rlim_t)opts->cpu_seconds) != 0)
		rc = -1;

	if (opts->memory_mb > 0 &&
	    set_limit(RLIMIT_AS, (rlim_t)opts->memory_mb * 1024 * 1024) != 0)
		rc = -1;

	if (opts->file_size_mb > 0 &&
	    set_limit(RLIMIT_FSIZE, (rlim_t)opts->file_size_mb * 1024 * 1024) != 0)
		rc = -1;

	if (opts->max_files > 0 &&
	    set_limit(RLIMIT_NOFILE, (rlim_t)opts->max_files) != 0)
		rc = -1;

	/* No cores. A crashing compiler on a 2 GB machine can otherwise write
	 * a dump larger than the workspace. */
	set_limit(RLIMIT_CORE, 0);

#if defined(__sgi)
	/*
	 * PR_MAXPROCS caps processes for this user, which is what actually
	 * stops a fork bomb. There is no rlimit equivalent on IRIX: RLIMIT_NPROC
	 * does not exist here.
	 */
	if (opts->max_procs > 0 &&
	    prctl(PR_MAXPROCS, (unsigned long)opts->max_procs) < 0)
		rc = -1;

	/*
	 * Kill anything the step leaves behind when it exits. Without this a
	 * backgrounded process outlives its job and holds the workspace open,
	 * so the next job's clean fails for no visible reason.
	 */
	prctl(PR_TERMCHILD);
#endif

	return rc;
}

void
sgug_confine_describe(const sgug_confine_opts *opts, char *out, size_t outlen)
{
	char buf[256];
	size_t used = 0;

	buf[0] = '\0';

	if (opts->cpu_seconds > 0)
		used += (size_t)sgug_snprintf(buf + used, sizeof(buf) - used,
		    "cpu %ds ", opts->cpu_seconds);
	if (opts->memory_mb > 0)
		used += (size_t)sgug_snprintf(buf + used, sizeof(buf) - used,
		    "mem %dM ", opts->memory_mb);
	if (opts->file_size_mb > 0)
		used += (size_t)sgug_snprintf(buf + used, sizeof(buf) - used,
		    "file %dM ", opts->file_size_mb);
	if (opts->max_files > 0)
		used += (size_t)sgug_snprintf(buf + used, sizeof(buf) - used,
		    "files %d ", opts->max_files);
#if defined(__sgi)
	if (opts->max_procs > 0)
		used += (size_t)sgug_snprintf(buf + used, sizeof(buf) - used,
		    "procs %d ", opts->max_procs);
#endif

	if (opts->chroot_dir != NULL && *opts->chroot_dir != '\0') {
		if (geteuid() == 0)
			sgug_snprintf(out, outlen, "%schroot %s uid %ld", buf,
			    opts->chroot_dir, (long)opts->uid);
		else
			sgug_snprintf(out, outlen,
			    "%sno chroot, the runner is not root", buf);
		return;
	}

	if (buf[0] == '\0')
		sgug_snprintf(out, outlen, "none");
	else
		sgug_snprintf(out, outlen, "%s", buf);
}
