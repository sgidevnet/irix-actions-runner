#ifndef SGUG_CRYPTO_AES_H
#define SGUG_CRYPTO_AES_H

#include <stddef.h>

/*
 * Decrypts one TaskAgentMessage body.
 *
 * key is the 32-byte session key recovered from the session's encryptionKey,
 * iv is 16 bytes. Both arrive base64 encoded on the wire; decode before
 * calling. Output is UTF-8 JSON, NUL terminated, and is never longer than the
 * ciphertext, so a ct_len-sized buffer always suffices.
 *
 * Returns the plaintext length excluding the terminator, or -1.
 *
 * Two details in here are not optional, and both come from the reference
 * runner writing the plaintext through a .NET StreamWriter:
 *
 *   - The plaintext starts with a UTF-8 BOM, which is stripped. A JSON parser
 *     fed those three bytes fails on the first token.
 *   - PKCS#7 padding is validated before removal rather than assumed. A message
 *     whose length lands on a block boundary carries a full trailing block of
 *     0x10, and treating the last byte as a count unconditionally would corrupt
 *     a message that legitimately ends in a byte valued 1 through 16.
 */
int sgug_aes_cbc_decrypt(const unsigned char *key, const unsigned char *iv,
    const unsigned char *ct, size_t ct_len, char *out, size_t outlen);

#endif /* SGUG_CRYPTO_AES_H */
