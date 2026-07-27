#include "crypto/aes.h"

#include <string.h>

#include <openssl/evp.h>

#define AES_BLOCK 16

static const unsigned char UTF8_BOM[3] = { 0xef, 0xbb, 0xbf };

int
sgug_aes_cbc_decrypt(const unsigned char *key, const unsigned char *iv,
    const unsigned char *ct, size_t ct_len, char *out, size_t outlen)
{
	EVP_CIPHER_CTX *ctx;
	unsigned char *buf = (unsigned char *)out;
	int n, total = 0;
	size_t pad, i, off;

	if (ct_len == 0 || ct_len % AES_BLOCK != 0 || outlen < ct_len + 1)
		return -1;

	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL)
		return -1;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1)
		goto fail;

	/* Unpad by hand below, so OpenSSL must not also try and fail. */
	EVP_CIPHER_CTX_set_padding(ctx, 0);

	if (EVP_DecryptUpdate(ctx, buf, &n, ct, (int)ct_len) != 1)
		goto fail;
	total = n;

	if (EVP_DecryptFinal_ex(ctx, buf + total, &n) != 1)
		goto fail;
	total += n;

	EVP_CIPHER_CTX_free(ctx);

	if (total < AES_BLOCK)
		return -1;

	pad = buf[total - 1];
	if (pad == 0 || pad > AES_BLOCK || (size_t)total < pad)
		return -1;
	for (i = 0; i < pad; i++) {
		if (buf[total - 1 - i] != pad)
			return -1;
	}
	total -= (int)pad;

	off = 0;
	if ((size_t)total >= sizeof(UTF8_BOM) &&
	    memcmp(buf, UTF8_BOM, sizeof(UTF8_BOM)) == 0)
		off = sizeof(UTF8_BOM);

	if (off != 0) {
		memmove(out, out + off, (size_t)total - off);
		total -= (int)off;
	}

	out[total] = '\0';
	return total;

fail:
	EVP_CIPHER_CTX_free(ctx);
	return -1;
}
