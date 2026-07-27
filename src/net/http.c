#include "net/http.h"

#include "net/tcp.h"
#include "net/tls.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REDIRECTS 10
#define MAX_HEADER_BYTES 65536
#define READ_CHUNK 16384

/*
 * Below this the Date header is dominated by its one second resolution, by
 * network latency, and by the server's own coarse clock. Correcting on small
 * values would inject error into a machine that is already right.
 */
#define SKEW_THRESHOLD 60

struct header {
	char *name;
	char *value;
};

struct sgug_http_resp {
	int status;
	struct header *headers;
	size_t nheaders;
	char *body;
	size_t body_len;
};

struct buf {
	char *p;
	size_t len;
	size_t cap;
};

struct sgug_http_client {
	sgug_tls_ctx *tls_ctx;
	char *user_agent;

	/* One kept-alive connection. */
	sgug_tls *conn;
	char *conn_host;
	int conn_port;

	sgug_time_t skew;
	int skew_valid;

	int (*abort_cb)(void *);
	void *abort_ctx;
};

/*
 * Bounds how long a single socket read blocks. The overall wait is still the
 * caller's timeout; this only sets how often we surface to check for shutdown.
 */
#define READ_SLICE_MS 2000

static pthread_key_t errkey;
static pthread_once_t err_once = PTHREAD_ONCE_INIT;

static void
free_errbuf(void *p)
{
	free(p);
}

static void
init_errkey(void)
{
	pthread_key_create(&errkey, free_errbuf);
}

static void
set_error(const char *fmt, const char *a)
{
	char *buf;

	pthread_once(&err_once, init_errkey);
	buf = pthread_getspecific(errkey);
	if (buf == NULL) {
		buf = malloc(256);
		if (buf == NULL)
			return;
		pthread_setspecific(errkey, buf);
	}
	sgug_snprintf(buf, 256, fmt, a);
}

void
sgug_http_set_abort_check(sgug_http_client *c, int (*cb)(void *), void *ctx)
{
	if (c == NULL)
		return;
	c->abort_cb = cb;
	c->abort_ctx = ctx;
}

const char *
sgug_http_last_error(void)
{
	const char *buf;

	pthread_once(&err_once, init_errkey);
	buf = pthread_getspecific(errkey);
	return buf != NULL ? buf : "";
}

/*
 * One read, retried across slice timeouts until the deadline or an abort.
 * Returns bytes read, 0 at EOF, or -1.
 */
static int
read_slice(sgug_http_client *c, void *buf, size_t len, int64_t deadline_ms)
{
	for (;;) {
		int n, pr, perr;

		/*
		 * Checked first, every iteration, and independent of poll's
		 * result. Deriving the decision from errno after poll is
		 * fragile: any intervening call may clobber it, and then a
		 * SIGTERM that interrupted the wait is silently ignored and the
		 * runner takes a full 50 second poll cycle to stop.
		 */
		if (c->abort_cb != NULL && c->abort_cb(c->abort_ctx) != 0) {
			set_error("aborted%s", "");
			return -1;
		}
		if (sgug_monotonic_ms() >= deadline_ms) {
			set_error("timed out waiting for response%s", "");
			return -1;
		}

		/*
		 * poll rather than SO_RCVTIMEO: IRIX 6.5 rejects that option
		 * with EPROTONOSUPPORT, so a socket-level read deadline does not
		 * exist there and a stalled peer would block forever.
		 *
		 * Skipped when OpenSSL already holds decrypted bytes, which the
		 * descriptor will never signal.
		 */
		if (sgug_tls_pending(c->conn) == 0) {
			struct pollfd pfd;

			pfd.fd = sgug_tls_fd(c->conn);
			pfd.events = POLLIN;
			pfd.revents = 0;

			pr = poll(&pfd, 1, READ_SLICE_MS);
			perr = pr < 0 ? errno : 0;

			if (pr == 0 || (pr < 0 && perr == EINTR))
				continue;
			if (pr < 0) {
				set_error("poll: %s", strerror(perr));
				return -1;
			}
		}

		n = sgug_tls_read(c->conn, buf, len);
		if (n != SGUG_TLS_TIMEOUT)
			return n;
	}
}

static int
buf_append(struct buf *b, const void *data, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t ncap = b->cap != 0 ? b->cap : 1024;
		char *np;

		while (ncap < b->len + n + 1)
			ncap *= 2;
		np = realloc(b->p, ncap);
		if (np == NULL)
			return -1;
		b->p = np;
		b->cap = ncap;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
	b->p[b->len] = '\0';
	return 0;
}

struct url {
	char scheme[8];
	char host[256];
	int port;
	char path[2048];
};

static int
parse_url(const char *url, struct url *u)
{
	const char *p, *host_start, *host_end, *colon, *slash;
	size_t n;

	memset(u, 0, sizeof(*u));

	p = strstr(url, "://");
	if (p == NULL)
		return -1;

	n = (size_t)(p - url);
	if (n >= sizeof(u->scheme))
		return -1;
	memcpy(u->scheme, url, n);
	u->scheme[n] = '\0';

	if (strcmp(u->scheme, "https") == 0)
		u->port = 443;
	else if (strcmp(u->scheme, "http") == 0)
		u->port = 80;
	else
		return -1;

	host_start = p + 3;
	slash = strchr(host_start, '/');
	host_end = slash != NULL ? slash : host_start + strlen(host_start);

	colon = memchr(host_start, ':', (size_t)(host_end - host_start));
	if (colon != NULL) {
		u->port = atoi(colon + 1);
		if (u->port <= 0 || u->port > 65535)
			return -1;
		host_end = colon;
	}

	n = (size_t)(host_end - host_start);
	if (n == 0 || n >= sizeof(u->host))
		return -1;
	memcpy(u->host, host_start, n);
	u->host[n] = '\0';

	if (slash != NULL) {
		if (strlen(slash) >= sizeof(u->path))
			return -1;
		strcpy(u->path, slash);
	} else {
		strcpy(u->path, "/");
	}
	return 0;
}

sgug_http_client *
sgug_http_client_new(const char *ca_bundle, const char *user_agent)
{
	sgug_http_client *c;

	if (user_agent == NULL || *user_agent == '\0') {
		set_error("user agent must not be empty%s", "");
		return NULL;
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;

	c->tls_ctx = sgug_tls_ctx_new(ca_bundle);
	if (c->tls_ctx == NULL) {
		set_error("tls context: %s", sgug_tls_last_error());
		free(c);
		return NULL;
	}

	c->user_agent = strdup(user_agent);
	if (c->user_agent == NULL) {
		sgug_tls_ctx_free(c->tls_ctx);
		free(c);
		return NULL;
	}
	return c;
}

static void
drop_conn(sgug_http_client *c)
{
	if (c->conn != NULL) {
		sgug_tls_free(c->conn);
		c->conn = NULL;
	}
	free(c->conn_host);
	c->conn_host = NULL;
	c->conn_port = 0;
}

void
sgug_http_client_free(sgug_http_client *c)
{
	if (c == NULL)
		return;
	drop_conn(c);
	sgug_tls_ctx_free(c->tls_ctx);
	free(c->user_agent);
	free(c);
}

static int
ensure_conn(sgug_http_client *c, const struct url *u, int timeout_ms)
{
	int fd;

	if (c->conn != NULL && c->conn_host != NULL &&
	    strcmp(c->conn_host, u->host) == 0 && c->conn_port == u->port)
		return 0;

	drop_conn(c);

	fd = sgug_tcp_connect(u->host, u->port, timeout_ms);
	if (fd < 0) {
		set_error("connect to %s failed", u->host);
		return -1;
	}
	sgug_tcp_set_nodelay(fd);
	/*
	 * Deliberately short, and unrelated to the caller's timeout: it decides
	 * how often a blocked read surfaces so shutdown can be noticed. The real
	 * deadline is enforced in read_slice.
	 */
	sgug_tcp_set_timeouts(fd, READ_SLICE_MS, timeout_ms);

	c->conn = sgug_tls_connect(c->tls_ctx, fd, u->host);
	if (c->conn == NULL) {
		set_error("tls: %s", sgug_tls_last_error());
		return -1;
	}

	c->conn_host = strdup(u->host);
	c->conn_port = u->port;
	return 0;
}

static void
resp_free_fields(struct sgug_http_resp *r)
{
	size_t i;

	for (i = 0; i < r->nheaders; i++) {
		free(r->headers[i].name);
		free(r->headers[i].value);
	}
	free(r->headers);
	free(r->body);
}

void
sgug_http_resp_free(sgug_http_resp *r)
{
	if (r == NULL)
		return;
	resp_free_fields(r);
	free(r);
}

static int
add_header(struct sgug_http_resp *r, const char *line, size_t len)
{
	const char *colon = memchr(line, ':', len);
	struct header *nh;
	size_t namelen, vallen;
	const char *val;

	if (colon == NULL)
		return 0;

	namelen = (size_t)(colon - line);
	val = colon + 1;
	vallen = len - namelen - 1;

	while (vallen > 0 && (*val == ' ' || *val == '\t')) {
		val++;
		vallen--;
	}

	nh = realloc(r->headers, (r->nheaders + 1) * sizeof(*nh));
	if (nh == NULL)
		return -1;
	r->headers = nh;

	r->headers[r->nheaders].name = malloc(namelen + 1);
	r->headers[r->nheaders].value = malloc(vallen + 1);
	if (r->headers[r->nheaders].name == NULL ||
	    r->headers[r->nheaders].value == NULL) {
		free(r->headers[r->nheaders].name);
		free(r->headers[r->nheaders].value);
		return -1;
	}

	memcpy(r->headers[r->nheaders].name, line, namelen);
	r->headers[r->nheaders].name[namelen] = '\0';
	memcpy(r->headers[r->nheaders].value, val, vallen);
	r->headers[r->nheaders].value[vallen] = '\0';
	r->nheaders++;
	return 0;
}

const char *
sgug_http_header(const sgug_http_resp *r, const char *name)
{
	size_t i;

	if (r == NULL || name == NULL)
		return NULL;

	for (i = 0; i < r->nheaders; i++) {
		const char *a = r->headers[i].name;
		const char *b = name;

		while (*a != '\0' && *b != '\0' &&
		    tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (*a == '\0' && *b == '\0')
			return r->headers[i].value;
	}
	return NULL;
}

int
sgug_http_status(const sgug_http_resp *r)
{
	return r != NULL ? r->status : 0;
}

const char *
sgug_http_body(const sgug_http_resp *r, size_t *len)
{
	if (r == NULL) {
		if (len != NULL)
			*len = 0;
		return "";
	}
	if (len != NULL)
		*len = r->body_len;
	return r->body != NULL ? r->body : "";
}

/* Reads until the buffer holds the end of the header block. */
static int
read_headers(sgug_http_client *c, struct buf *b, size_t *hdr_end,
    int64_t deadline_ms)
{
	for (;;) {
		char tmp[READ_CHUNK];
		char *p;
		int n;

		p = b->p != NULL ? strstr(b->p, "\r\n\r\n") : NULL;
		if (p != NULL) {
			*hdr_end = (size_t)(p - b->p) + 4;
			return 0;
		}
		if (b->len > MAX_HEADER_BYTES) {
			set_error("response headers too large%s", "");
			return -1;
		}

		n = read_slice(c, tmp, sizeof(tmp), deadline_ms);
		if (n < 0)
			return -1;
		if (n == 0) {
			set_error("connection closed before headers%s", "");
			return -1;
		}
		if (buf_append(b, tmp, (size_t)n) != 0)
			return -1;
	}
}

static int
read_exact(sgug_http_client *c, struct buf *b, size_t have, size_t want,
    int64_t deadline_ms)
{
	while (have < want) {
		char tmp[READ_CHUNK];
		size_t need = want - have;
		int n;

		if (need > sizeof(tmp))
			need = sizeof(tmp);

		n = read_slice(c, tmp, need, deadline_ms);
		if (n <= 0) {
			if (n == 0)
				set_error("truncated body%s", "");
			return -1;
		}
		if (buf_append(b, tmp, (size_t)n) != 0)
			return -1;
		have += (size_t)n;
	}
	return 0;
}

/*
 * Decodes chunked transfer coding in place. The service uses it for timeline
 * and job responses whose length is not known when the headers are sent.
 */
static int
decode_chunked(sgug_http_client *c, struct buf *b, size_t start,
    struct buf *out, int64_t deadline_ms)
{
	size_t pos = start;

	for (;;) {
		char *line_end;
		unsigned long size;
		size_t line_len;

		/* Pull more until a full size line is present. */
		while ((line_end = strstr(b->p + pos, "\r\n")) == NULL) {
			char tmp[READ_CHUNK];
			int n = read_slice(c, tmp, sizeof(tmp), deadline_ms);

			if (n <= 0) {
				if (n == 0)
					set_error("truncated chunk header%s", "");
				return -1;
			}
			if (buf_append(b, tmp, (size_t)n) != 0)
				return -1;
		}

		size = strtoul(b->p + pos, NULL, 16);
		line_len = (size_t)(line_end - (b->p + pos)) + 2;
		pos += line_len;

		if (size == 0)
			return 0;

		if (read_exact(c, b, b->len - pos, size + 2, deadline_ms) != 0)
			return -1;

		if (buf_append(out, b->p + pos, size) != 0)
			return -1;
		pos += size + 2;
	}
}

static void
update_skew(sgug_http_client *c, const sgug_http_resp *r)
{
	const char *date = sgug_http_header(r, "Date");
	sgug_time_t server;

	if (date == NULL)
		return;

	server = sgug_parse_http_date(date);
	if (server < 0)
		return;

	c->skew = sgug_now() - server;
	c->skew_valid = 1;
}

sgug_time_t
sgug_http_skew(const sgug_http_client *c)
{
	return c != NULL && c->skew_valid ? c->skew : 0;
}

sgug_time_t
sgug_http_now(const sgug_http_client *c)
{
	sgug_time_t now = sgug_now();
	sgug_time_t skew = sgug_http_skew(c);

	if (skew > SKEW_THRESHOLD || skew < -SKEW_THRESHOLD)
		return now - skew;
	return now;
}

static int
do_request(sgug_http_client *c, const char *method, const struct url *u,
    const char *const *headers, size_t nheaders,
    const void *body, size_t body_len, int timeout_ms,
    struct sgug_http_resp *r, int retry_ok)
{
	struct buf raw, decoded;
	char head[4096];
	int64_t deadline = sgug_monotonic_ms() + timeout_ms;
	size_t i, off = 0, hdr_end = 0;
	const char *cl, *te;
	char *line, *nl;
	int rc = -1;

	memset(&raw, 0, sizeof(raw));
	memset(&decoded, 0, sizeof(decoded));

	if (ensure_conn(c, u, timeout_ms) != 0)
		return -1;

	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "%s %s HTTP/1.1\r\n", method, u->path);
	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "Host: %s\r\n", u->host);
	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "User-Agent: %s\r\n", c->user_agent);
	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
	    "Accept-Encoding: identity\r\n");

	for (i = 0; i < nheaders; i++) {
		off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
		    "%s\r\n", headers[i]);
	}

	if (body != NULL && body_len > 0) {
		off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
		    "Content-Length: %lu\r\n", (unsigned long)body_len);
	} else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
	    strcmp(method, "PATCH") == 0) {
		/* The registration handshake POSTs an empty body, and the
		 * service wants the length stated rather than absent. */
		off += (size_t)sgug_snprintf(head + off, sizeof(head) - off,
		    "Content-Length: 0\r\n");
	}

	off += (size_t)sgug_snprintf(head + off, sizeof(head) - off, "\r\n");

	if (sgug_tls_write(c->conn, head, off) < 0 ||
	    (body != NULL && body_len > 0 &&
	    sgug_tls_write(c->conn, body, body_len) < 0)) {
		/*
		 * A kept-alive connection the server closed while idle fails on
		 * the first write. That is expected, not an error, so reconnect
		 * once and replay. The request has not been seen, so this is
		 * safe even for non-idempotent methods.
		 */
		drop_conn(c);
		if (retry_ok)
			return do_request(c, method, u, headers, nheaders,
			    body, body_len, timeout_ms, r, 0);
		set_error("write: %s", sgug_tls_last_error());
		return -1;
	}

	if (read_headers(c, &raw, &hdr_end, deadline) != 0) {
		drop_conn(c);
		if (retry_ok && raw.len == 0)
			return do_request(c, method, u, headers, nheaders,
			    body, body_len, timeout_ms, r, 0);
		goto out;
	}

	if (strncmp(raw.p, "HTTP/1.", 7) != 0) {
		set_error("not an HTTP response%s", "");
		goto out;
	}
	r->status = atoi(raw.p + 9);

	line = strstr(raw.p, "\r\n");
	if (line == NULL)
		goto out;
	line += 2;

	while (line < raw.p + hdr_end - 2) {
		nl = strstr(line, "\r\n");
		if (nl == NULL || nl == line)
			break;
		if (add_header(r, line, (size_t)(nl - line)) != 0)
			goto out;
		line = nl + 2;
	}

	te = sgug_http_header(r, "Transfer-Encoding");
	cl = sgug_http_header(r, "Content-Length");

	if (te != NULL && strcasestr(te, "chunked") != NULL) {
		if (decode_chunked(c, &raw, hdr_end, &decoded, deadline) != 0)
			goto out;
		r->body = decoded.p;
		r->body_len = decoded.len;
		decoded.p = NULL;
	} else if (cl != NULL) {
		size_t want = (size_t)strtoul(cl, NULL, 10);
		size_t have = raw.len - hdr_end;

		if (read_exact(c, &raw, have, want, deadline) != 0)
			goto out;

		r->body = malloc(want + 1);
		if (r->body == NULL)
			goto out;
		memcpy(r->body, raw.p + hdr_end, want);
		r->body[want] = '\0';
		r->body_len = want;
	} else if (r->status == 204 || r->status == 304 ||
	    strcmp(method, "HEAD") == 0) {
		/* 204 is the long poll's "no message"; it carries no body and
		 * no length, and waiting for EOF here would stall the loop. */
		r->body = strdup("");
		r->body_len = 0;
	} else {
		/* No length and no chunking means the body ends at EOF. */
		for (;;) {
			char tmp[READ_CHUNK];
			int n = read_slice(c, tmp, sizeof(tmp), deadline);

			if (n < 0)
				goto out;
			if (n == 0)
				break;
			if (buf_append(&raw, tmp, (size_t)n) != 0)
				goto out;
		}
		r->body_len = raw.len - hdr_end;
		r->body = malloc(r->body_len + 1);
		if (r->body == NULL)
			goto out;
		memcpy(r->body, raw.p + hdr_end, r->body_len);
		r->body[r->body_len] = '\0';
		drop_conn(c);
	}

	{
		const char *conn_hdr = sgug_http_header(r, "Connection");

		if (conn_hdr != NULL && strcasestr(conn_hdr, "close") != NULL)
			drop_conn(c);
	}

	update_skew(c, r);
	rc = 0;

out:
	free(raw.p);
	free(decoded.p);
	return rc;
}

int
sgug_http_request(sgug_http_client *c, const char *method, const char *url,
    const char *const *headers, size_t nheaders,
    const void *body, size_t body_len, int timeout_ms,
    sgug_http_resp **out)
{
	char current[2048];
	int redirects;

	if (c == NULL || out == NULL)
		return -1;
	*out = NULL;

	if (strlen(url) >= sizeof(current)) {
		set_error("url too long%s", "");
		return -1;
	}
	strcpy(current, url);

	for (redirects = 0; redirects <= MAX_REDIRECTS; redirects++) {
		struct sgug_http_resp *r;
		struct url u;
		const char *loc;

		if (parse_url(current, &u) != 0) {
			set_error("bad url: %s", current);
			return -1;
		}

		r = calloc(1, sizeof(*r));
		if (r == NULL)
			return -1;

		if (do_request(c, method, &u, headers, nheaders, body, body_len,
		    timeout_ms, r, 1) != 0) {
			sgug_http_resp_free(r);
			return -1;
		}

		if (r->status != 301 && r->status != 302 && r->status != 303 &&
		    r->status != 307 && r->status != 308) {
			*out = r;
			return 0;
		}

		loc = sgug_http_header(r, "Location");
		if (loc == NULL || strlen(loc) >= sizeof(current)) {
			*out = r;
			return 0;
		}

		if (strstr(loc, "://") != NULL) {
			strcpy(current, loc);
		} else {
			/* Origin-relative Location. */
			char tmp[2048];

			sgug_snprintf(tmp, sizeof(tmp), "%s://%s%s", u.scheme,
			    u.host, loc);
			strcpy(current, tmp);
		}

		sgug_http_resp_free(r);
	}

	set_error("too many redirects%s", "");
	return -1;
}
