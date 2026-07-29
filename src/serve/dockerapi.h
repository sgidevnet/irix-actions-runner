#ifndef SGUG_SERVE_DOCKERAPI_H
#define SGUG_SERVE_DOCKERAPI_H

#include <stddef.h>

/*
 * The subset of the Docker Engine API the container executor needs. The
 * socket is /var/run/docker.sock, or DOCKER_HOST when that names a unix://
 * path; any other form is an error rather than a silent fallback.
 */

typedef struct {
	const char *key;
	const char *value;
} sgug_docker_label;

typedef struct {
	char id[72];
	char supervisor[24];
	char staging[512];
} sgug_docker_container;

/*
 * bind is one HostConfig Binds entry and env holds NAME=VALUE strings. An
 * image the daemon does not have is pulled. A container that fails to start
 * is removed before returning.
 */
int sgug_docker_run(const char *image, const char *bind,
    const char *const *env, size_t nenv,
    const sgug_docker_label *labels, size_t nlabel,
    char *id, size_t idlen, char *err, size_t errlen);

int sgug_docker_inspect(const char *id, int *running, int *code,
    char *err, size_t errlen);

int sgug_docker_kill(const char *id, char *err, size_t errlen);

int sgug_docker_remove(const char *id, char *err, size_t errlen);

/*
 * Containers carrying label sup_key, running or not, up to max of them.
 * supervisor and staging hold those two labels, empty where absent.
 * truncated is set when more than max matched.
 */
int sgug_docker_list(const char *sup_key, const char *stage_key,
    sgug_docker_container *out, size_t max, size_t *n, int *truncated,
    char *err, size_t errlen);

#endif /* SGUG_SERVE_DOCKERAPI_H */
