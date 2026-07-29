#ifndef SGUG_SERVE_DOCKERAPI_H
#define SGUG_SERVE_DOCKERAPI_H

#include <stddef.h>

/*
 * The subset of the Docker Engine API the container executor needs, spoken
 * over the unix socket. Every call is one HTTP/1.1 request. Failures return -1
 * and fill err with the daemon's own message where it sent one.
 *
 * The socket is /var/run/docker.sock, or DOCKER_HOST when that names a
 * unix:// path. Any other DOCKER_HOST form is an error rather than a silent
 * fallback to the default.
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
 * Creates a container and starts it, writing its id to id. bind is one
 * HostConfig Binds entry and env holds NAME=VALUE strings. A container that
 * fails to start is removed before returning.
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
 * supervisor and staging hold the values of sup_key and stage_key, empty when
 * the container does not carry them.
 */
int sgug_docker_list(const char *sup_key, const char *stage_key,
    sgug_docker_container *out, size_t max, size_t *n,
    char *err, size_t errlen);

#endif /* SGUG_SERVE_DOCKERAPI_H */
