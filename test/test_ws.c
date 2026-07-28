/*
 * WebSocket framing and the handshake key. No sockets: everything here is a
 * pure function over buffers, which is the only part of ws.c that can be
 * covered without a server to talk to.
 */

#include "net/ws.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_INT(got, want) \
	do { \
		long g_ = (long)(got), w_ = (long)(want); \
		if (g_ != w_) { \
			printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, \
			    __LINE__, #got, g_, w_); \
			failures++; \
		} \
	} while (0)

/* RFC 6455 section 1.3. */
static void
test_accept_key(void)
{
	char out[40];

	CHECK_EQ_INT(sgug_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out,
	    sizeof(out)), 0);
	CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
	if (strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != 0)
		printf("      got \"%s\"\n", out);
}

static void
test_accept_key_rejects_small_buffer(void)
{
	char out[8];

	CHECK(sgug_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out,
	    sizeof(out)) != 0);
}

/*
 * The three length forms. A server may reject a longer encoding carrying a
 * value that fits a shorter one, so the boundaries matter more than the sizes.
 */
static void
test_frame_lengths(void)
{
	static const unsigned char MASK[4] = { 0xde, 0xad, 0xbe, 0xef };
	unsigned char h[SGUG_WS_MAX_HEADER];
	size_t n;

	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 0, MASK);
	CHECK_EQ_INT(n, 6);
	CHECK_EQ_INT(h[0], 0x81);
	CHECK_EQ_INT(h[1], 0x80);

	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 125, MASK);
	CHECK_EQ_INT(n, 6);
	CHECK_EQ_INT(h[1], 0x80 | 125);

	/* 126 is the first that needs the 16-bit form. */
	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 126, MASK);
	CHECK_EQ_INT(n, 8);
	CHECK_EQ_INT(h[1], 0x80 | 126);
	CHECK_EQ_INT(h[2], 0);
	CHECK_EQ_INT(h[3], 126);

	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 65535, MASK);
	CHECK_EQ_INT(n, 8);
	CHECK_EQ_INT(h[2], 0xff);
	CHECK_EQ_INT(h[3], 0xff);

	/* 65536 is the first that needs the 64-bit form. Big endian, so the
	 * whole of 0x0000000000010000 is spelled out across h[2]..h[9]. */
	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 65536, MASK);
	CHECK_EQ_INT(n, 14);
	CHECK_EQ_INT(h[1], 0x80 | 127);
	CHECK_EQ_INT(h[2], 0x00);
	CHECK_EQ_INT(h[3], 0x00);
	CHECK_EQ_INT(h[4], 0x00);
	CHECK_EQ_INT(h[5], 0x00);
	CHECK_EQ_INT(h[6], 0x00);
	CHECK_EQ_INT(h[7], 0x01);
	CHECK_EQ_INT(h[8], 0x00);
	CHECK_EQ_INT(h[9], 0x00);

	/* One past the 32-bit boundary, to prove the shift is done in 64 bits.
	 * On n32 a long is 32 bits, so this is exactly where a careless
	 * implementation loses the high word. */
	n = sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 0x100000000ULL, MASK);
	CHECK_EQ_INT(n, 14);
	CHECK_EQ_INT(h[5], 0x01);
	CHECK_EQ_INT(h[6], 0x00);
	CHECK_EQ_INT(h[9], 0x00);
}

/* The mask bit is mandatory on every client frame, whatever the opcode. */
static void
test_mask_bit_and_opcodes(void)
{
	static const unsigned char MASK[4] = { 1, 2, 3, 4 };
	unsigned char h[SGUG_WS_MAX_HEADER];

	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 4, MASK);
	CHECK_EQ_INT(h[0], 0x81);
	CHECK(h[1] & 0x80);

	/* A non-final fragment clears FIN and keeps the opcode. */
	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 0, 4, MASK);
	CHECK_EQ_INT(h[0], 0x01);

	/* Continuations carry opcode 0. */
	sgug_ws_frame_header(h, SGUG_WS_OP_CONT, 0, 4, MASK);
	CHECK_EQ_INT(h[0], 0x00);
	sgug_ws_frame_header(h, SGUG_WS_OP_CONT, 1, 4, MASK);
	CHECK_EQ_INT(h[0], 0x80);

	sgug_ws_frame_header(h, SGUG_WS_OP_CLOSE, 1, 2, MASK);
	CHECK_EQ_INT(h[0], 0x88);
	sgug_ws_frame_header(h, SGUG_WS_OP_PONG, 1, 0, MASK);
	CHECK_EQ_INT(h[0], 0x8a);

	/* The key follows the length, wherever the length ended. */
	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 200, MASK);
	CHECK_EQ_INT(h[4], 1);
	CHECK_EQ_INT(h[7], 4);
}

static void
test_mask_round_trip(void)
{
	static const unsigned char MASK[4] = { 0x37, 0xfa, 0x21, 0x3d };
	char buf[32];

	memcpy(buf, "Hello", 5);
	sgug_ws_mask((unsigned char *)buf, 5, MASK);
	CHECK(memcmp(buf, "Hello", 5) != 0);
	sgug_ws_mask((unsigned char *)buf, 5, MASK);
	CHECK(memcmp(buf, "Hello", 5) == 0);
}

/* RFC 6455 section 5.7: "Hello" under that key is a published vector. */
static void
test_mask_vector(void)
{
	static const unsigned char MASK[4] = { 0x37, 0xfa, 0x21, 0x3d };
	static const unsigned char WANT[5] = { 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	unsigned char buf[5];

	memcpy(buf, "Hello", 5);
	sgug_ws_mask(buf, 5, MASK);
	CHECK(memcmp(buf, WANT, 5) == 0);
}

/*
 * Fragmentation. A message exactly one fragment long must stay a single frame:
 * an empty trailing continuation is legal but the reference never emits one.
 */
static void
test_fragmentation(void)
{
	size_t chunk;
	int fin, opcode;

	sgug_ws_next_fragment(0, 0, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 0);
	CHECK_EQ_INT(fin, 1);
	CHECK_EQ_INT(opcode, SGUG_WS_OP_TEXT);

	sgug_ws_next_fragment(1, 0, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1);
	CHECK_EQ_INT(fin, 1);

	sgug_ws_next_fragment(1023, 0, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1023);
	CHECK_EQ_INT(fin, 1);

	/* Exactly one fragment: one frame, FIN set, no continuation. */
	sgug_ws_next_fragment(1024, 0, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1024);
	CHECK_EQ_INT(fin, 1);
	CHECK_EQ_INT(opcode, SGUG_WS_OP_TEXT);

	/* One byte more needs a continuation, and only the second is final. */
	sgug_ws_next_fragment(1025, 0, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1024);
	CHECK_EQ_INT(fin, 0);
	CHECK_EQ_INT(opcode, SGUG_WS_OP_TEXT);
	sgug_ws_next_fragment(1025, 1024, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1);
	CHECK_EQ_INT(fin, 1);
	CHECK_EQ_INT(opcode, SGUG_WS_OP_CONT);

	/* A three-fragment message: only the last carries FIN. */
	sgug_ws_next_fragment(2048, 1024, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1024);
	CHECK_EQ_INT(fin, 1);
	CHECK_EQ_INT(opcode, SGUG_WS_OP_CONT);
	sgug_ws_next_fragment(3000, 1024, &chunk, &fin, &opcode);
	CHECK_EQ_INT(chunk, 1024);
	CHECK_EQ_INT(fin, 0);
}

/* The masking key follows the length, wherever the length happened to end. */
static void
test_mask_key_position(void)
{
	static const unsigned char MASK[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
	unsigned char h[SGUG_WS_MAX_HEADER];

	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 10, MASK);
	CHECK_EQ_INT(h[2], 0xaa);
	CHECK_EQ_INT(h[5], 0xdd);

	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 1000, MASK);
	CHECK_EQ_INT(h[4], 0xaa);
	CHECK_EQ_INT(h[7], 0xdd);

	sgug_ws_frame_header(h, SGUG_WS_OP_TEXT, 1, 0x100000000ULL, MASK);
	CHECK_EQ_INT(h[10], 0xaa);
	CHECK_EQ_INT(h[13], 0xdd);
}

int
main(void)
{
	test_accept_key();
	test_accept_key_rejects_small_buffer();
	test_frame_lengths();
	test_mask_bit_and_opcodes();
	test_mask_round_trip();
	test_mask_vector();
	test_fragmentation();
	test_mask_key_position();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all ws tests passed\n");
	return 0;
}
