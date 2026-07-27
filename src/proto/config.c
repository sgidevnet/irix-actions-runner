#include "proto/config.h"

#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void
sgug_config_path(const char *dir, const char *name, char *out, size_t outlen)
{
	sgug_snprintf(out, outlen, "%s/%s", dir, name);
}

/* Creates at the target mode rather than chmod'ing afterwards, so a credential
 * file is never momentarily readable by anyone else. */
static int
write_file(const char *path, const char *data, size_t len, mode_t mode)
{
	int fd;
	ssize_t n;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;

	n = write(fd, data, len);
	if (close(fd) != 0 || n < 0 || (size_t)n != len) {
		unlink(path);
		return -1;
	}
	return 0;
}

static char *
read_file(const char *path, size_t *out_len)
{
	struct stat st;
	char *buf;
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) != 0 || st.st_size < 0) {
		close(fd);
		return NULL;
	}

	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		close(fd);
		return NULL;
	}

	n = read(fd, buf, (size_t)st.st_size);
	close(fd);

	if (n < 0) {
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	if (out_len != NULL)
		*out_len = (size_t)n;
	return buf;
}

int
sgug_config_save(const sgug_config *cfg, const char *dir)
{
	sgug_jsonw *w;
	char path[SGUG_MAX_URL];
	const char *json;
	size_t len;
	int rc = -1;

	/* .runner */
	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "AgentId");
	sgug_jsonw_int(w, cfg->agent_id);
	sgug_jsonw_key(w, "AgentName");
	sgug_jsonw_str(w, cfg->agent_name);
	sgug_jsonw_key(w, "PoolId");
	sgug_jsonw_int(w, cfg->pool_id);
	sgug_jsonw_key(w, "PoolName");
	sgug_jsonw_str(w, cfg->pool_name);
	sgug_jsonw_key(w, "ServerUrl");
	sgug_jsonw_str(w, cfg->server_url);
	sgug_jsonw_key(w, "GitHubUrl");
	sgug_jsonw_str(w, cfg->github_url);
	sgug_jsonw_key(w, "WorkFolder");
	sgug_jsonw_str(w, cfg->work_folder);
	sgug_jsonw_key(w, "Ephemeral");
	sgug_jsonw_bool(w, cfg->ephemeral);
	sgug_jsonw_key(w, "DisableUpdate");
	sgug_jsonw_bool(w, cfg->disable_update);
	sgug_jsonw_key(w, "UseV2Flow");
	sgug_jsonw_bool(w, cfg->use_v2_flow);
	sgug_jsonw_key(w, "ServerUrlV2");
	sgug_jsonw_str(w, cfg->server_url_v2);
	sgug_jsonw_obj_end(w);

	json = sgug_jsonw_done(w, &len);
	if (json == NULL)
		goto out;

	sgug_config_path(dir, ".runner", path, sizeof(path));
	if (write_file(path, json, len, 0644) != 0)
		goto out;

	sgug_jsonw_free(w);

	/* .credentials */
	w = sgug_jsonw_new();
	if (w == NULL)
		return -1;

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "Scheme");
	sgug_jsonw_str(w, "OAuth");
	sgug_jsonw_key(w, "Data");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "clientId");
	sgug_jsonw_str(w, cfg->client_id);
	sgug_jsonw_key(w, "authorizationUrl");
	sgug_jsonw_str(w, cfg->auth_url);
	/* Written as a quoted string, matching the service's own
	 * Dictionary<string,string> representation. */
	sgug_jsonw_key(w, "requireFipsCryptography");
	sgug_jsonw_str(w, cfg->require_fips ? "True" : "False");
	if (cfg->auth_url_v2[0] != '\0') {
		sgug_jsonw_key(w, "authorizationUrlV2");
		sgug_jsonw_str(w, cfg->auth_url_v2);
	}
	sgug_jsonw_obj_end(w);
	sgug_jsonw_obj_end(w);

	json = sgug_jsonw_done(w, &len);
	if (json == NULL)
		goto out;

	sgug_config_path(dir, ".credentials", path, sizeof(path));
	if (write_file(path, json, len, 0600) != 0)
		goto out;

	rc = 0;

out:
	sgug_jsonw_free(w);
	return rc;
}

int
sgug_config_load(sgug_config *cfg, const char *dir)
{
	char path[SGUG_MAX_URL];
	char *text;
	sgug_json_doc *doc;
	const sgug_json *r, *data;
	size_t len;

	memset(cfg, 0, sizeof(*cfg));

	sgug_config_path(dir, ".runner", path, sizeof(path));
	text = read_file(path, &len);
	if (text == NULL)
		return -1;

	doc = sgug_json_parse(text, len, NULL, 0);
	free(text);
	if (doc == NULL)
		return -1;

	r = sgug_json_root(doc);
	cfg->agent_id = sgug_json_int(sgug_json_get(r, "AgentId"), 0);
	cfg->pool_id = sgug_json_int(sgug_json_get(r, "PoolId"), 0);
	cfg->ephemeral = sgug_json_bool(sgug_json_get(r, "Ephemeral"), 0);
	cfg->disable_update = sgug_json_bool(sgug_json_get(r, "DisableUpdate"), 1);
	cfg->use_v2_flow = sgug_json_bool(sgug_json_get(r, "UseV2Flow"), 0);

	sgug_snprintf(cfg->agent_name, sizeof(cfg->agent_name), "%s",
	    sgug_json_string(sgug_json_get(r, "AgentName"), ""));
	sgug_snprintf(cfg->pool_name, sizeof(cfg->pool_name), "%s",
	    sgug_json_string(sgug_json_get(r, "PoolName"), ""));
	sgug_snprintf(cfg->server_url, sizeof(cfg->server_url), "%s",
	    sgug_json_string(sgug_json_get(r, "ServerUrl"), ""));
	sgug_snprintf(cfg->server_url_v2, sizeof(cfg->server_url_v2), "%s",
	    sgug_json_string(sgug_json_get(r, "ServerUrlV2"), ""));
	sgug_snprintf(cfg->github_url, sizeof(cfg->github_url), "%s",
	    sgug_json_string(sgug_json_get(r, "GitHubUrl"), ""));
	sgug_snprintf(cfg->work_folder, sizeof(cfg->work_folder), "%s",
	    sgug_json_string(sgug_json_get(r, "WorkFolder"), "_work"));

	sgug_json_free(doc);

	sgug_config_path(dir, ".credentials", path, sizeof(path));
	text = read_file(path, &len);
	if (text == NULL)
		return -1;

	doc = sgug_json_parse(text, len, NULL, 0);
	free(text);
	if (doc == NULL)
		return -1;

	data = sgug_json_get(sgug_json_root(doc), "Data");
	sgug_snprintf(cfg->client_id, sizeof(cfg->client_id), "%s",
	    sgug_json_string(sgug_json_get(data, "clientId"), ""));
	sgug_snprintf(cfg->auth_url, sizeof(cfg->auth_url), "%s",
	    sgug_json_string(sgug_json_get(data, "authorizationUrl"), ""));
	sgug_snprintf(cfg->auth_url_v2, sizeof(cfg->auth_url_v2), "%s",
	    sgug_json_string(sgug_json_get(data, "authorizationUrlV2"), ""));
	cfg->require_fips = sgug_json_bool(
	    sgug_json_get(data, "requireFipsCryptography"), 0);

	sgug_json_free(doc);

	return cfg->client_id[0] != '\0' && cfg->server_url[0] != '\0' ? 0 : -1;
}

int
sgug_config_exists(const char *dir)
{
	char path[SGUG_MAX_URL];
	struct stat st;

	sgug_config_path(dir, ".runner", path, sizeof(path));
	return stat(path, &st) == 0;
}

int
sgug_config_remove(const char *dir)
{
	static const char *FILES[] = { ".runner", ".credentials", ".rsakey" };
	char path[SGUG_MAX_URL];
	size_t i;
	int rc = 0;

	for (i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
		sgug_config_path(dir, FILES[i], path, sizeof(path));
		if (unlink(path) != 0 && errno != ENOENT)
			rc = -1;
	}
	return rc;
}
