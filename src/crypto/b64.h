#ifndef SGUG_CRYPTO_B64_H
#define SGUG_CRYPTO_B64_H

#include <stddef.h>

/*
 * The protocol uses both alphabets and is strict about which goes where.
 * Standard, padded: RSA modulus and exponent in the agent registration, the
 * session key, message IVs and bodies. URL-safe, unpadded: every JWT segment.
 * Sending one where the other belongs fails authentication with no useful
 * diagnostic, so they are separate calls rather than a flag.
 */

/* Encode to standard base64 with padding. Returns bytes written excluding the
 * terminator, or -1 if out is too small. Needs 4*((len+2)/3)+1 bytes. */
int sgug_b64_encode(const void *src, size_t len, char *out, size_t outlen);

/* Encode to URL-safe base64 without padding, per RFC 4648 section 5. */
int sgug_b64url_encode(const void *src, size_t len, char *out, size_t outlen);

/*
 * Decode either alphabet, with or without padding. Returns bytes written, or
 * -1 on an invalid character or truncated group. Output is never longer than
 * the input, so an input-sized buffer always suffices.
 */
int sgug_b64_decode(const char *src, void *out, size_t outlen);

/* Bytes needed by sgug_b64_encode, including the terminator. */
size_t sgug_b64_encoded_size(size_t len);

#endif /* SGUG_CRYPTO_B64_H */
