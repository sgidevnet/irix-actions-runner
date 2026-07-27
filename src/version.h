#ifndef SGUG_VERSION_H
#define SGUG_VERSION_H

/*
 * Reported to the Actions service at registration and on every long poll.
 *
 * The service enforces a minimum and refuses to dispatch to a runner below it,
 * so this tracks GitHub's current release rather than our own progress. Bump it
 * when the floor rises; nothing else depends on the value.
 */
#define SGUG_RUNNER_VERSION "2.336.0"

#define SGUG_USER_AGENT \
	"irix-actions-runner/" SGUG_RUNNER_VERSION " (IRIX; mips)"

/*
 * The OS this runner claims to be.
 *
 * "Linux" by default and deliberately untrue. GitHub's system label vocabulary
 * has no MIPS or IRIX value, and ordinary workflow YAML says
 * `runs-on: [self-hosted, linux, x64]` and branches on
 * `runner.os == 'Linux'`, so claiming Linux is what makes existing workflows
 * schedulable and behave.
 *
 * Override with SGUG_RUNNER_OS to find out what the service does with an
 * honest answer. It appears in the acquirejob request, the message poll query
 * and the RUNNER_OS variable given to steps.
 */
const char *sgug_runner_os(void);

#endif /* SGUG_VERSION_H */
