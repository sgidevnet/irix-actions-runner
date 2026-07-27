#ifndef SGUG_PROTO_CONFIG_H
#define SGUG_PROTO_CONFIG_H

#include "compat/irix.h"

#include <stddef.h>

/*
 * Persisted runner identity and credentials.
 *
 * Split across three files in the runner directory, mirroring the reference
 * runner's layout so an operator can recognise them:
 *
 *   .runner       identity and endpoints, world readable
 *   .credentials  OAuth client id and token endpoint, mode 0600
 *   .rsakey       the private key, mode 0600
 *
 * The key is PKCS#1 PEM rather than the reference runner's
 * .credentials_rsaparams, which is a Newtonsoft dump of base64 fields. Nothing
 * on the wire depends on the file format.
 */

#define SGUG_MAX_URL 512
#define SGUG_MAX_NAME 128

typedef struct {
	/* .runner */
	char server_url[SGUG_MAX_URL];		/* TenantUrl, the _apis base */
	char server_url_v2[SGUG_MAX_URL];	/* broker base, empty on v1 */
	char github_url[SGUG_MAX_URL];
	char agent_name[SGUG_MAX_NAME];
	char pool_name[SGUG_MAX_NAME];
	char work_folder[SGUG_MAX_NAME];
	int64_t agent_id;
	int64_t pool_id;
	int ephemeral;
	int disable_update;
	int use_v2_flow;

	/* .credentials */
	char client_id[64];
	char auth_url[SGUG_MAX_URL];
	char auth_url_v2[SGUG_MAX_URL];
	/*
	 * When set the OAuth assertion must be PS256 rather than RS256. The
	 * service reports it as the string "True" inside a
	 * Dictionary<string,string>, so it is read with sgug_json_bool.
	 */
	int require_fips;
} sgug_config;

/*
 * dir is the runner directory. Both writes create their files 0600 before any
 * content reaches them, so credentials are never briefly world readable.
 */
int sgug_config_save(const sgug_config *cfg, const char *dir);
int sgug_config_load(sgug_config *cfg, const char *dir);

/* True if dir holds a usable configuration. */
int sgug_config_exists(const char *dir);

/* Removes .runner, .credentials and .rsakey. */
int sgug_config_remove(const char *dir);

/* Builds "<dir>/<name>" into out. */
void sgug_config_path(const char *dir, const char *name, char *out, size_t outlen);

#endif /* SGUG_PROTO_CONFIG_H */
