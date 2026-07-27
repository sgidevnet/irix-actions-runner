#include "crypto/b64.h"

#include <string.h>

static const char STD_ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL_ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* 0..63 decode value, 64 for '=', -1 for anything else. Accepts both
 * alphabets, since '-'/'_' and '+'/'/' never collide. */
static int
decode_char(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	if (c == '=')
		return 64;
	return -1;
}

size_t
sgug_b64_encoded_size(size_t len)
{
	return 4 * ((len + 2) / 3) + 1;
}

static int
encode(const void *src, size_t len, char *out, size_t outlen,
    const char *alphabet, int pad)
{
	const unsigned char *p = src;
	size_t i, o = 0;
	size_t need;

	need = pad ? 4 * ((len + 2) / 3) : (len * 4 + 2) / 3;
	if (outlen < need + 1)
		return -1;

	for (i = 0; i + 3 <= len; i += 3) {
		unsigned long v = ((unsigned long)p[i] << 16) |
		    ((unsigned long)p[i + 1] << 8) | p[i + 2];

		out[o++] = alphabet[(v >> 18) & 0x3f];
		out[o++] = alphabet[(v >> 12) & 0x3f];
		out[o++] = alphabet[(v >> 6) & 0x3f];
		out[o++] = alphabet[v & 0x3f];
	}

	if (i < len) {
		size_t rem = len - i;
		unsigned long v = (unsigned long)p[i] << 16;

		if (rem == 2)
			v |= (unsigned long)p[i + 1] << 8;

		out[o++] = alphabet[(v >> 18) & 0x3f];
		out[o++] = alphabet[(v >> 12) & 0x3f];
		if (rem == 2)
			out[o++] = alphabet[(v >> 6) & 0x3f];
		else if (pad)
			out[o++] = '=';
		if (pad)
			out[o++] = '=';
	}

	out[o] = '\0';
	return (int)o;
}

int
sgug_b64_encode(const void *src, size_t len, char *out, size_t outlen)
{
	return encode(src, len, out, outlen, STD_ALPHABET, 1);
}

int
sgug_b64url_encode(const void *src, size_t len, char *out, size_t outlen)
{
	return encode(src, len, out, outlen, URL_ALPHABET, 0);
}

int
sgug_b64_decode(const char *src, void *out, size_t outlen)
{
	unsigned char *dst = out;
	unsigned long acc = 0;
	int nbits = 0;
	size_t o = 0;

	for (; *src != '\0'; src++) {
		int v = decode_char((unsigned char)*src);

		if (v < 0)
			return -1;
		if (v == 64)
			break;

		acc = (acc << 6) | (unsigned long)v;
		nbits += 6;

		if (nbits >= 8) {
			nbits -= 8;
			if (o >= outlen)
				return -1;
			dst[o++] = (unsigned char)((acc >> nbits) & 0xff);
		}
	}

	/*
	 * A trailing group of 6 leftover bits cannot come from any whole byte,
	 * so the input was truncated. Leftover bits must also be zero; a
	 * non-canonical encoding means the input was not produced by an encoder
	 * and we should not silently accept it.
	 */
	if (nbits >= 6)
		return -1;
	if (nbits > 0 && (acc & ((1UL << nbits) - 1)) != 0)
		return -1;

	return (int)o;
}
