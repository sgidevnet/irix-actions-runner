/*
 * Docker Engine API over the unix socket. One connection per request: the
 * daemon honours Connection: close, so a response is read to EOF and decoded
 * in memory rather than incrementally.
 */

#include "serve/dockerapi.h"

#include "compat/irix.h"
#include "json/json.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A floor, not the daemon's version: it serves any path at or below its own. */
#define API_VERSION "/v1.43"

#define DEFAULT_SOCKET "/var/run/docker.sock"
#define MAX_RESP (1024 * 1024)

struct buf {
	char *p;
	size_t len;
	size_t cap;
};

static int
oom(char *err, size_t errlen)
{
	sgug_snprintf(err, errlen, "out of memory");
	return -1;
}

static int
buf_add(struct buf *b, const void *data, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap != 0 ? b->cap : 4096;
		char *np;

		while (cap < b->len + n + 1)
			cap *= 2;
		np = realloc(b->p, cap);
		if (np == NULL)
			return -1;
		b->p = np;
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
	b->p[b->len] = '\0';
	return 0;
}

static int
docker_connect(char *err, size_t errlen)
{
	struct sockaddr_un sa;
	const char *host = getenv("DOCKER_HOST");
	const char *path = DEFAULT_SOCKET;
	int fd;

	if (host != NULL && *host != '\0') {
		if (strncmp(host, "unix://", 7) != 0) {
			sgug_snprintf(err, errlen, "DOCKER_HOST %s is not a "
			    "unix:// socket", host);
			return -1;
		}
		path = host + 7;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path)) {
		sgug_snprintf(err, errlen, "socket path %s is too long", path);
		return -1;
	}
	strcpy(sa.sun_path, path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		sgug_snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		sgug_snprintf(err, errlen, "connect %s: %s", path,
		    strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int
write_all(int fd, const void *data, size_t len)
{
	const char *p = data;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);

		/* Handlers go in without SA_RESTART. */
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/* Value of a header within a NUL terminated header block, or NULL. */
static const char *
header_value(const char *head, const char *name)
{
	char pat[32];
	const char *p;

	sgug_snprintf(pat, sizeof(pat), "\r\n%s:", name);
	p = strcasestr(head, pat);
	if (p == NULL)
		return NULL;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static int
decode_chunked(const char *p, const char *end, struct buf *out, char *err,
    size_t errlen)
{
	for (;;) {
		const char *nl = memmem(p, (size_t)(end - p), "\r\n", 2);
		unsigned long size;

		if (nl == NULL) {
			sgug_snprintf(err, errlen, "truncated chunk header");
			return -1;
		}
		size = strtoul(p, NULL, 16);
		p = nl + 2;
		if (size == 0)
			return 0;
		if ((size_t)(end - p) < size + 2) {
			sgug_snprintf(err, errlen, "truncated chunk body");
			return -1;
		}
		if (buf_add(out, p, size) != 0)
			return oom(err, errlen);
		p += size + 2;
	}
}

static int
parse_response(struct buf *raw, int *status, struct buf *out, char *err,
    size_t errlen)
{
	char *hdr_end;
	const char *te, *cl;
	size_t off, len;

	if (raw->len < 12 || strncmp(raw->p, "HTTP/1.", 7) != 0) {
		sgug_snprintf(err, errlen, "not an HTTP response");
		return -1;
	}
	hdr_end = strstr(raw->p, "\r\n\r\n");
	if (hdr_end == NULL) {
		sgug_snprintf(err, errlen, "truncated response headers");
		return -1;
	}
	*status = atoi(raw->p + 9);

	off = (size_t)(hdr_end - raw->p) + 4;
	/* Terminate after the last header's own CRLF so a body that happens to
	 * spell a header name cannot be matched. */
	hdr_end[2] = '\0';

	te = header_value(raw->p, "Transfer-Encoding");
	cl = header_value(raw->p, "Content-Length");

	if (te != NULL && strcasestr(te, "chunked") != NULL)
		return decode_chunked(raw->p + off, raw->p + raw->len, out,
		    err, errlen);

	len = raw->len - off;
	if (cl != NULL) {
		size_t want = (size_t)strtoul(cl, NULL, 10);

		if (len < want) {
			sgug_snprintf(err, errlen, "truncated body");
			return -1;
		}
		len = want;
	}
	if (buf_add(out, raw->p + off, len) != 0)
		return oom(err, errlen);
	return 0;
}

static int
request(const char *method, const char *path, const char *body, int *status,
    struct buf *out, char *err, size_t errlen)
{
	struct buf raw;
	char head[1024];
	size_t off = 0;
	int fd, rc = -1;

	memset(&raw, 0, sizeof(raw));

	fd = docker_connect(err, errlen);
	if (fd < 0)
		return -1;

	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "%s " API_VERSION "%s HTTP/1.1\r\n", method, path);
	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "Host: docker\r\nAccept: application/json\r\n"
	    "Connection: close\r\n");
	if (body != NULL)
		off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
		    "Content-Type: application/json\r\n");
	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "Content-Length: %lu\r\n\r\n",
	    body != NULL ? (unsigned long)strlen(body) : 0UL);

	if (write_all(fd, head, off) != 0 ||
	    (body != NULL && write_all(fd, body, strlen(body)) != 0)) {
		sgug_snprintf(err, errlen, "write: %s", strerror(errno));
		goto out;
	}

	for (;;) {
		char tmp[8192];
		ssize_t n = read(fd, tmp, sizeof(tmp));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			sgug_snprintf(err, errlen, "read: %s", strerror(errno));
			goto out;
		}
		if (n == 0)
			break;
		if (raw.len + (size_t)n > MAX_RESP) {
			sgug_snprintf(err, errlen, "response exceeds %lu "
			    "bytes", (unsigned long)MAX_RESP);
			goto out;
		}
		if (buf_add(&raw, tmp, (size_t)n) != 0) {
			oom(err, errlen);
			goto out;
		}
	}

	rc = parse_response(&raw, status, out, err, errlen);
out:
	free(raw.p);
	close(fd);
	return rc;
}

/* The daemon states its reason in {"message": ...} on everything it rejects. */
static void
fail_status(const char *what, int status, const struct buf *b, char *err,
    size_t errlen)
{
	char scratch[128];
	sgug_json_doc *doc = b->p != NULL ?
	    sgug_json_parse(b->p, b->len, scratch, sizeof(scratch)) : NULL;
	const char *msg = sgug_json_string(
	    sgug_json_get(sgug_json_root(doc), "message"), "");

	sgug_snprintf(err, errlen, "%s: HTTP %d%s%s", what, status,
	    *msg != '\0' ? ": " : "", msg);
	sgug_json_free(doc);
}

static int
urlencode(const char *s, char *out, size_t outlen)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;

	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' ||
		    c == '~') {
			if (o + 1 >= outlen)
				return -1;
			out[o++] = (char)c;
			continue;
		}
		if (o + 3 >= outlen)
			return -1;
		out[o++] = '%';
		out[o++] = hex[c >> 4];
		out[o++] = hex[c & 0x0f];
	}
	out[o] = '\0';
	return 0;
}

int
sgug_docker_run(const char *image, const char *bind, const char *const *env,
    size_t nenv, const sgug_docker_label *labels, size_t nlabel, char *id,
    size_t idlen, char *err, size_t errlen)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_json_doc *doc = NULL;
	struct buf resp;
	char path[128], scratch[128];
	const char *json;
	size_t i;
	int status, rc = -1;

	memset(&resp, 0, sizeof(resp));
	if (w == NULL)
		return oom(err, errlen);

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "Image");
	sgug_jsonw_str(w, image);
	sgug_jsonw_key(w, "Env");
	sgug_jsonw_arr_begin(w);
	for (i = 0; i < nenv; i++)
		sgug_jsonw_str(w, env[i]);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_key(w, "Labels");
	sgug_jsonw_obj_begin(w);
	for (i = 0; i < nlabel; i++) {
		sgug_jsonw_key(w, labels[i].key);
		sgug_jsonw_str(w, labels[i].value);
	}
	sgug_jsonw_obj_end(w);
	sgug_jsonw_key(w, "HostConfig");
	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "Binds");
	sgug_jsonw_arr_begin(w);
	sgug_jsonw_str(w, bind);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);
	sgug_jsonw_obj_end(w);

	json = sgug_jsonw_done(w, NULL);
	if (json == NULL) {
		oom(err, errlen);
		goto out;
	}

	if (request("POST", "/containers/create", json, &status, &resp, err,
	    errlen) != 0)
		goto out;
	if (status != 201) {
		fail_status("create", status, &resp, err, errlen);
		goto out;
	}

	doc = sgug_json_parse(resp.p, resp.len, err, errlen);
	if (doc == NULL)
		goto out;
	sgug_snprintf(id, idlen, "%s",
	    sgug_json_string(sgug_json_get(sgug_json_root(doc), "Id"), ""));
	if (id[0] == '\0') {
		sgug_snprintf(err, errlen, "create returned no container id");
		goto out;
	}

	free(resp.p);
	memset(&resp, 0, sizeof(resp));
	sgug_snprintf(path, sizeof(path), "/containers/%s/start", id);

	if (request("POST", path, NULL, &status, &resp, err, errlen) != 0) {
		sgug_docker_remove(id, scratch, sizeof(scratch));
		goto out;
	}
	if (status != 204) {
		fail_status("start", status, &resp, err, errlen);
		sgug_docker_remove(id, scratch, sizeof(scratch));
		goto out;
	}
	rc = 0;
out:
	sgug_json_free(doc);
	sgug_jsonw_free(w);
	free(resp.p);
	return rc;
}

int
sgug_docker_inspect(const char *id, int *running, int *code, char *err,
    size_t errlen)
{
	sgug_json_doc *doc = NULL;
	struct buf resp;
	char path[128];
	const sgug_json *state;
	int status, rc = -1;

	memset(&resp, 0, sizeof(resp));
	sgug_snprintf(path, sizeof(path), "/containers/%s/json", id);

	if (request("GET", path, NULL, &status, &resp, err, errlen) != 0)
		goto out;
	if (status != 200) {
		fail_status("inspect", status, &resp, err, errlen);
		goto out;
	}

	doc = sgug_json_parse(resp.p, resp.len, err, errlen);
	if (doc == NULL)
		goto out;

	state = sgug_json_get(sgug_json_root(doc), "State");
	if (state == NULL) {
		sgug_snprintf(err, errlen, "inspect: no State");
		goto out;
	}
	*running = sgug_json_bool(sgug_json_get(state, "Running"), 0);
	*code = (int)sgug_json_int(sgug_json_get(state, "ExitCode"), 0);
	rc = 0;
out:
	sgug_json_free(doc);
	free(resp.p);
	return rc;
}

int
sgug_docker_kill(const char *id, char *err, size_t errlen)
{
	struct buf resp;
	char path[128];
	int status, rc = -1;

	memset(&resp, 0, sizeof(resp));
	sgug_snprintf(path, sizeof(path), "/containers/%s/kill", id);

	if (request("POST", path, NULL, &status, &resp, err, errlen) == 0) {
		if (status == 204)
			rc = 0;
		else
			fail_status("kill", status, &resp, err, errlen);
	}
	free(resp.p);
	return rc;
}

int
sgug_docker_remove(const char *id, char *err, size_t errlen)
{
	struct buf resp;
	char path[128];
	int status, rc = -1;

	memset(&resp, 0, sizeof(resp));
	sgug_snprintf(path, sizeof(path), "/containers/%s?force=1", id);

	if (request("DELETE", path, NULL, &status, &resp, err, errlen) == 0) {
		if (status == 204)
			rc = 0;
		else
			fail_status("remove", status, &resp, err, errlen);
	}
	free(resp.p);
	return rc;
}

int
sgug_docker_list(const char *sup_key, const char *stage_key,
    sgug_docker_container *out, size_t max, size_t *n, char *err,
    size_t errlen)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_json_doc *doc = NULL;
	struct buf resp;
	char path[512], filters[384];
	const sgug_json *arr;
	const char *json;
	size_t i, count;
	int status, rc = -1;

	*n = 0;
	memset(&resp, 0, sizeof(resp));
	if (w == NULL)
		return oom(err, errlen);

	sgug_jsonw_obj_begin(w);
	sgug_jsonw_key(w, "label");
	sgug_jsonw_arr_begin(w);
	sgug_jsonw_str(w, sup_key);
	sgug_jsonw_arr_end(w);
	sgug_jsonw_obj_end(w);

	json = sgug_jsonw_done(w, NULL);
	if (json == NULL) {
		oom(err, errlen);
		goto out;
	}
	if (urlencode(json, filters, sizeof(filters)) != 0) {
		sgug_snprintf(err, errlen, "label filter is too long");
		goto out;
	}
	sgug_snprintf(path, sizeof(path), "/containers/json?all=1&filters=%s",
	    filters);

	if (request("GET", path, NULL, &status, &resp, err, errlen) != 0)
		goto out;
	if (status != 200) {
		fail_status("list", status, &resp, err, errlen);
		goto out;
	}

	doc = sgug_json_parse(resp.p, resp.len, err, errlen);
	if (doc == NULL)
		goto out;

	arr = sgug_json_root(doc);
	count = sgug_json_len(arr);
	if (count > max)
		count = max;

	for (i = 0; i < count; i++) {
		const sgug_json *c = sgug_json_at(arr, i);
		const sgug_json *labels = sgug_json_get(c, "Labels");

		sgug_snprintf(out[i].id, sizeof(out[i].id), "%s",
		    sgug_json_string(sgug_json_get(c, "Id"), ""));
		sgug_snprintf(out[i].supervisor, sizeof(out[i].supervisor),
		    "%s", sgug_json_string(sgug_json_get(labels, sup_key), ""));
		sgug_snprintf(out[i].staging, sizeof(out[i].staging), "%s",
		    sgug_json_string(sgug_json_get(labels, stage_key), ""));
	}
	*n = count;
	rc = 0;
out:
	sgug_json_free(doc);
	sgug_jsonw_free(w);
	free(resp.p);
	return rc;
}
