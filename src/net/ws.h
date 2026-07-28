#ifndef SGUG_NET_WS_H
#define SGUG_NET_WS_H

#include "net/tls.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A send-only WebSocket client, RFC 6455, for the Actions live console feed.
 *
 * The official runner never reads from this socket: it opens it, writes batches
 * of console lines, and closes it. This does the same. Nothing the server sends
 * has to be parsed, and a socket the peer has closed surfaces as a failed send,
 * so there is no receive path to keep in sync with the sender.
 *
 * The feed is best effort by design. Every failure is terminal for the socket
 * and harmless for the job: the uploaded log remains the authoritative copy.
 */

typedef struct sgug_ws sgug_ws;

/*
 * Connects to a wss:// URL and completes the upgrade handshake. Borrows ctx,
 * which must outlive the socket. Returns NULL and fills err on failure.
 *
 * token, when not NULL, is sent as `Authorization: Bearer <token>`.
 */
sgug_ws *sgug_ws_connect(sgug_tls_ctx *ctx, const char *url, const char *token,
    const char *user_agent, int timeout_ms, char *err, size_t errlen);

/*
 * Sends one text message. Returns 0, or -1 with err filled, after which the
 * socket must not be used again.
 */
int sgug_ws_send_text(sgug_ws *ws, const char *text, size_t len, int timeout_ms,
    char *err, size_t errlen);

/* Sends a close frame, best effort. The reply is not awaited. */
void sgug_ws_close(sgug_ws *ws);

void sgug_ws_free(sgug_ws *ws);

/*
 * Below here is exposed for unit tests: the framing has no I/O in it, and a
 * socket cannot be mocked in this tree.
 */

/* Largest header a client frame can need: 2 + 8 length + 4 mask. */
#define SGUG_WS_MAX_HEADER 14

/* The fragment size the reference client uses. Frames are split at this. */
#define SGUG_WS_FRAGMENT 1024

#define SGUG_WS_OP_CONT 0x0
#define SGUG_WS_OP_TEXT 0x1
#define SGUG_WS_OP_CLOSE 0x8
#define SGUG_WS_OP_PING 0x9
#define SGUG_WS_OP_PONG 0xa

/*
 * Writes a client frame header into out, which must hold SGUG_WS_MAX_HEADER
 * bytes, and returns its length. The mask is applied by sgug_ws_mask.
 */
size_t sgug_ws_frame_header(unsigned char *out, int opcode, int fin,
    uint64_t payload_len, const unsigned char mask[4]);

/* XOR with the masking key. Self-inverse, so it also unmasks. Each frame is
 * masked independently, so the key always restarts at the frame's first byte. */
void sgug_ws_mask(unsigned char *buf, size_t len, const unsigned char mask[4]);

/*
 * base64(SHA1(key + the RFC 6455 GUID)), the value a server must return in
 * Sec-WebSocket-Accept. Writes 28 characters, so out needs 29 bytes.
 */
int sgug_ws_accept_key(const char *key, char *out, size_t outlen);

/*
 * Describes the fragment of a message starting at off: its length, whether it
 * is the last, and which opcode carries it. Split out so the fragmentation can
 * be tested without a socket.
 */
void sgug_ws_next_fragment(size_t len, size_t off, size_t *chunk, int *fin,
    int *opcode);

#endif /* SGUG_NET_WS_H */
