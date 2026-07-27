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

#endif /* SGUG_VERSION_H */
