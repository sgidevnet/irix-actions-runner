#include "net/ws.h"

#include "compat/irix.h"
#include "crypto/b64.h"
#include "net/tcp.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* RFC 6455 section 1.3. Concatenated with the client key to derive the accept
 * value, and the only reason a SHA-1 lives in this tree. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define READ_SLICE_MS 500

struct sgug_ws {
	sgug_tls *conn;
	int closed;
};

static void
seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;

	va_start(ap, fmt);
	sgug_vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

size_t
sgug_ws_frame_header(unsigned char *out, int opcode, int fin,
    uint64_t payload_len, const unsigned char mask[4])
{
	size_t n = 0;

	out[n++] = (unsigned char)((fin ? 0x80 : 0x00) | (opcode & 0x0f));

	/* Shortest of the three forms: a longer one carrying a small value is
	 * legal to reject. */
	if (payload_len < 126) {
		out[n++] = (unsigned char)(0x80 | payload_len);
	} else if (payload_len <= 0xffff) {
		out[n++] = 0x80 | 126;
		out[n++] = (unsigned char)((payload_len >> 8) & 0xff);
		out[n++] = (unsigned char)(payload_len & 0xff);
	} else {
		int i;

		out[n++] = 0x80 | 127;
		for (i = 7; i >= 0; i--)
			out[n++] = (unsigned char)((payload_len >> (i * 8)) & 0xff);
	}

	memcpy(out + n, mask, 4);
	return n + 4;
}

void
sgug_ws_mask(unsigned char *buf, size_t len, const unsigned char mask[4])
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] ^= mask[i & 3];
}

void
sgug_ws_next_fragment(size_t len, size_t off, size_t *chunk, int *fin,
    int *opcode)
{
	size_t n = len - off;

	if (n > SGUG_WS_FRAGMENT)
		n = SGUG_WS_FRAGMENT;

	*chunk = n;
	/* A message exactly one fragment long is one frame, not a frame plus an
	 * empty continuation. */
	*fin = off + n >= len;
	*opcode = off == 0 ? SGUG_WS_OP_TEXT : SGUG_WS_OP_CONT;
}

int
sgug_ws_accept_key(const char *key, char *out, size_t outlen)
{
	EVP_MD_CTX *ctx;
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;
	int rc = -1;

	ctx = EVP_MD_CTX_new();
	if (ctx == NULL)
		return -1;

	if (EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 &&
	    EVP_DigestUpdate(ctx, key, strlen(key)) == 1 &&
	    EVP_DigestUpdate(ctx, WS_GUID, sizeof(WS_GUID) - 1) == 1 &&
	    EVP_DigestFinal_ex(ctx, md, &mdlen) == 1)
		rc = sgug_b64_encode(md, mdlen, out, outlen) < 0 ? -1 : 0;

	EVP_MD_CTX_free(ctx);
	return rc;
}

/* wss://host[:port]/path. Only wss: the feed is never offered over plain ws. */
static int
parse_url(const char *url, char *host, size_t hostlen, int *port,
    char *path, size_t pathlen, char *err, size_t errlen)
{
	const char *p, *slash, *colon;
	size_t n;

	if (strncmp(url, "wss://", 6) != 0) {
		seterr(err, errlen, "not a wss:// url");
		return -1;
	}
	p = url + 6;

	slash = strchr(p, '/');
	n = slash != NULL ? (size_t)(slash - p) : strlen(p);
	if (n == 0 || n >= hostlen) {
		seterr(err, errlen, "bad host in url");
		return -1;
	}
	memcpy(host, p, n);
	host[n] = '\0';

	*port = 443;
	colon = strchr(host, ':');
	if (colon != NULL) {
		*(char *)colon = '\0';
		*port = atoi(colon + 1);
		if (*port <= 0 || *port > 65535) {
			seterr(err, errlen, "bad port in url");
			return -1;
		}
	}

	p = slash != NULL ? slash : "/";
	if (strlen(p) >= pathlen) {
		seterr(err, errlen, "url path too long");
		return -1;
	}
	sgug_snprintf(path, pathlen, "%s", p);
	return 0;
}

/* Blocking read of one slice, bounded by the deadline. Mirrors http.c's
 * read_slice: SSL may hold a whole frame that never makes the fd readable. */
static int
read_some(sgug_ws *ws, void *buf, size_t len, int64_t deadline_ms)
{
	for (;;) {
		struct pollfd pfd;
		int n, pr;

		if (sgug_monotonic_ms() >= deadline_ms)
			return -1;

		if (sgug_tls_pending(ws->conn) == 0) {
			pfd.fd = sgug_tls_fd(ws->conn);
			pfd.events = POLLIN;
			pfd.revents = 0;

			pr = poll(&pfd, 1, READ_SLICE_MS);
			if (pr == 0 || (pr < 0 && errno == EINTR))
				continue;
			if (pr < 0)
				return -1;
		}

		n = sgug_tls_read(ws->conn, buf, len);
		if (n != SGUG_TLS_TIMEOUT)
			return n;
	}
}

static int
handshake(sgug_ws *ws, const char *host, const char *path, const char *token,
    const char *user_agent, int64_t deadline_ms, char *err, size_t errlen)
{
	unsigned char nonce[16];
	/* The job token is a JWT carrying scopes and runs to a couple of
	 * kilobytes. Sized so the Authorization header cannot be truncated:
	 * sgug_snprintf terminates rather than reporting overflow, so a short
	 * buffer here sends a malformed request and the server just hangs up. */
	char key[32], want[40], req[8192], resp[2048];
	size_t off = 0, have = 0;
	char *eoh, *accept;

	if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
		seterr(err, errlen, "no entropy for the websocket key");
		return -1;
	}
	if (sgug_b64_encode(nonce, sizeof(nonce), key, sizeof(key)) < 0 ||
	    sgug_ws_accept_key(key, want, sizeof(want)) != 0) {
		seterr(err, errlen, "cannot derive the websocket key");
		return -1;
	}

	off += (size_t)sgug_snprintf(req + off, sizeof(req) - off,
	    "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
	    "Sec-WebSocket-Version: 13\r\n", path, host, key);
	if (user_agent != NULL && *user_agent != '\0')
		off += (size_t)sgug_snprintf(req + off, sizeof(req) - off,
		    "User-Agent: %s\r\n", user_agent);
	if (token != NULL && *token != '\0')
		off += (size_t)sgug_snprintf(req + off, sizeof(req) - off,
		    "Authorization: Bearer %s\r\n", token);
	off += (size_t)sgug_snprintf(req + off, sizeof(req) - off, "\r\n");

	/* sgug_snprintf truncates rather than reporting overflow, so a token
	 * larger than the buffer would send a request with no header
	 * terminator and the server would simply hang up. */
	if (off < 4 || memcmp(req + off - 4, "\r\n\r\n", 4) != 0) {
		seterr(err, errlen, "upgrade request does not fit in %lu bytes",
		    (unsigned long)sizeof(req));
		return -1;
	}

	if (sgug_tls_write(ws->conn, req, off) < 0) {
		seterr(err, errlen, "upgrade request: %s", sgug_tls_last_error());
		return -1;
	}

	/*
	 * Read until the header terminator. Anything past it would be server
	 * frames, which cannot appear before we have sent one.
	 */
	for (;;) {
		int n;

		if (have + 1 >= sizeof(resp)) {
			seterr(err, errlen, "upgrade response headers too long");
			return -1;
		}
		n = read_some(ws, resp + have, sizeof(resp) - have - 1,
		    deadline_ms);
		if (n <= 0) {
			seterr(err, errlen, "upgrade response: %s",
			    n == 0 ? "connection closed" : "timed out");
			return -1;
		}
		have += (size_t)n;
		resp[have] = '\0';

		eoh = strstr(resp, "\r\n\r\n");
		if (eoh != NULL)
			break;
	}

	if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
		char *eol = strchr(resp, '\r');

		if (eol != NULL)
			*eol = '\0';
		seterr(err, errlen, "upgrade refused: %.120s", resp);
		return -1;
	}

	accept = strcasestr(resp, "sec-websocket-accept:");
	if (accept == NULL) {
		seterr(err, errlen, "upgrade response carried no accept key");
		return -1;
	}
	accept += sizeof("sec-websocket-accept:") - 1;
	while (*accept == ' ' || *accept == '\t')
		accept++;

	{
		size_t wlen = strlen(want);
		char end;

		end = accept[wlen];
		if (strncmp(accept, want, wlen) != 0 ||
		    (end != '\r' && end != '\n' && end != ' ' && end != '\t')) {
			seterr(err, errlen, "upgrade accept key did not match");
			return -1;
		}
	}
	return 0;
}

sgug_ws *
sgug_ws_connect(sgug_tls_ctx *ctx, const char *url, const char *token,
    const char *user_agent, int timeout_ms, char *err, size_t errlen)
{
	sgug_ws *ws;
	char host[256], path[512];
	int port, fd;

	if (ctx == NULL || url == NULL) {
		seterr(err, errlen, "no websocket url");
		return NULL;
	}
	if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), err,
	    errlen) != 0)
		return NULL;

	ws = calloc(1, sizeof(*ws));
	if (ws == NULL) {
		seterr(err, errlen, "out of memory");
		return NULL;
	}

	fd = sgug_tcp_connect(host, port, timeout_ms);
	if (fd < 0) {
		seterr(err, errlen, "connect to %s failed", host);
		free(ws);
		return NULL;
	}
	sgug_tcp_set_nodelay(fd);
	sgug_tcp_set_timeouts(fd, READ_SLICE_MS, timeout_ms);

	ws->conn = sgug_tls_connect(ctx, fd, host);
	if (ws->conn == NULL) {
		/* Ownership of fd transfers only on success, and two of the
		 * failure paths run before it is handed over at all. */
		seterr(err, errlen, "tls: %s", sgug_tls_last_error());
		close(fd);
		free(ws);
		return NULL;
	}

	if (handshake(ws, host, path, token, user_agent,
	    sgug_monotonic_ms() + timeout_ms, err, errlen) != 0) {
		sgug_ws_free(ws);
		return NULL;
	}
	return ws;
}

/*
 * IRIX rejects SO_SNDTIMEO, so the socket is blocking and a peer that stops
 * reading would stall the thread draining the child's pipe, wedging the step.
 * Bounds the common case; a stall mid-frame is still possible.
 */
static int
wait_writable(sgug_ws *ws, int timeout_ms)
{
	struct pollfd pfd;
	int64_t deadline = sgug_monotonic_ms() + timeout_ms;

	for (;;) {
		int pr;

		pfd.fd = sgug_tls_fd(ws->conn);
		pfd.events = POLLOUT;
		pfd.revents = 0;

		pr = poll(&pfd, 1, READ_SLICE_MS);
		if (pr > 0)
			return 0;
		if (pr < 0 && errno != EINTR)
			return -1;
		if (sgug_monotonic_ms() >= deadline)
			return -1;
	}
}

static int
send_frame(sgug_ws *ws, int opcode, int fin, const void *payload, size_t len,
    char *err, size_t errlen)
{
	unsigned char head[SGUG_WS_MAX_HEADER];
	unsigned char mask[4];
	unsigned char *body = NULL;
	size_t headlen;
	int rc = -1;

	if (RAND_bytes(mask, sizeof(mask)) != 1) {
		seterr(err, errlen, "no entropy for the frame mask");
		return -1;
	}
	headlen = sgug_ws_frame_header(head, opcode, fin, (uint64_t)len, mask);

	if (len > 0) {
		body = malloc(len);
		if (body == NULL) {
			seterr(err, errlen, "out of memory");
			return -1;
		}
		memcpy(body, payload, len);
		sgug_ws_mask(body, len, mask);
	}

	/*
	 * Header and payload go in separate writes. Nagle is off, so this costs
	 * a segment; copying them into one buffer would cost a second copy of
	 * every log line.
	 */
	if (sgug_tls_write(ws->conn, head, headlen) < 0 ||
	    (len > 0 && sgug_tls_write(ws->conn, body, len) < 0))
		seterr(err, errlen, "send: %s", sgug_tls_last_error());
	else
		rc = 0;

	free(body);
	return rc;
}

int
sgug_ws_send_text(sgug_ws *ws, const char *text, size_t len, int timeout_ms,
    char *err, size_t errlen)
{
	size_t off = 0;

	if (ws == NULL || ws->closed) {
		seterr(err, errlen, "socket is closed");
		return -1;
	}

	if (wait_writable(ws, timeout_ms) != 0) {
		seterr(err, errlen, "socket did not accept a write in %d ms",
		    timeout_ms);
		ws->closed = 1;
		return -1;
	}

	/*
	 * Fragmented at the size the reference client uses. A single frame would
	 * be legal, but matching the only client this server is known to accept
	 * removes a class of question that is expensive to answer remotely.
	 */
	do {
		size_t chunk;
		int fin, opcode;

		sgug_ws_next_fragment(len, off, &chunk, &fin, &opcode);

		if (send_frame(ws, opcode, fin, text + off, chunk, err,
		    errlen) != 0) {
			ws->closed = 1;
			return -1;
		}
		off += chunk;
	} while (off < len);

	return 0;
}

void
sgug_ws_close(sgug_ws *ws)
{
	unsigned char payload[2];
	char err[128];

	if (ws == NULL || ws->closed)
		return;

	/* 1000, normal closure. The reply is not awaited: the runner has nothing
	 * left to say and the job must not wait on a socket it no longer needs. */
	payload[0] = 0x03;
	payload[1] = 0xe8;
	send_frame(ws, SGUG_WS_OP_CLOSE, 1, payload, sizeof(payload), err,
	    sizeof(err));
	ws->closed = 1;
}

void
sgug_ws_free(sgug_ws *ws)
{
	if (ws == NULL)
		return;
	sgug_tls_free(ws->conn);
	free(ws);
}
