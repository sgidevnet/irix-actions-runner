#ifndef SGUG_SANDBOX_CONFINE_H
#define SGUG_SANDBOX_CONFINE_H

#include <sys/types.h>

/*
 * Confinement applied in the child, between fork and exec.
 *
 * Mostly this is about accidents rather than adversaries. A parallel build that
 * forks without bound, a test that resolves a path wrong and writes into the
 * home directory, a link step that allocates until the machine swaps: these are
 * ordinary Tuesday failures on a shared 2 GB workstation, and each one is
 * cheap to bound. GitHub's fork-PR approval setting covers the hostile case.
 *
 * IRIX offers no namespaces, no seccomp and no jails. chroot with an
 * unprivileged uid, resource limits and the sproc process caps are the whole
 * toolbox, so this uses all of it.
 *
 * Every layer is optional and reported rather than assumed, because the ones
 * needing privilege are unavailable when the runner is started by an ordinary
 * user, which is the normal case.
 */

typedef struct {
	/* Seconds of CPU per step. 0 disables. */
	int cpu_seconds;
	/* Address space ceiling in megabytes. 0 disables. */
	int memory_mb;
	/* Largest file a step may create, in megabytes. 0 disables. */
	int file_size_mb;
	/* Open descriptors. 0 disables. */
	int max_files;
	/* Processes for this user. 0 disables. This is the fork bomb cap. */
	int max_procs;

	/*
	 * Root only. When set, the child chroots here and drops to the uid and
	 * gid below. Ignored with a warning when the runner is not root.
	 */
	const char *chroot_dir;
	uid_t uid;
	gid_t gid;
} sgug_confine_opts;

/*
 * Applies everything possible and returns 0, or -1 if a limit that was asked
 * for could not be set. Call only in the child: several of these are
 * irreversible.
 *
 * Never fails merely because a privileged layer was unavailable; that is
 * reported through sgug_confine_describe instead.
 */
int sgug_confine_apply(const sgug_confine_opts *opts);

/*
 * One line naming what would actually be enforced for these options on this
 * machine, for the job log. Written before the fork, so an operator can see
 * the difference between confinement being off and being unavailable.
 */
void sgug_confine_describe(const sgug_confine_opts *opts, char *out,
    size_t outlen);

/* Defaults sized for a 2816 MB Octane running one job at a time. */
void sgug_confine_defaults(sgug_confine_opts *opts);

#endif /* SGUG_SANDBOX_CONFINE_H */
