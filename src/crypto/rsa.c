#include "crypto/rsa.h"

#include "compat/irix.h"
#include "crypto/b64.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#define RSA_BITS 2048

struct sgug_rsa {
	RSA *rsa;
};

sgug_rsa *
sgug_rsa_generate(void)
{
	sgug_rsa *k;
	BIGNUM *e;

	k = calloc(1, sizeof(*k));
	if (k == NULL)
		return NULL;

	e = BN_new();
	if (e == NULL || BN_set_word(e, RSA_F4) != 1) {
		BN_free(e);
		free(k);
		return NULL;
	}

	k->rsa = RSA_new();
	if (k->rsa == NULL || RSA_generate_key_ex(k->rsa, RSA_BITS, e, NULL) != 1) {
		RSA_free(k->rsa);
		BN_free(e);
		free(k);
		return NULL;
	}

	BN_free(e);
	return k;
}

void
sgug_rsa_free(sgug_rsa *k)
{
	if (k == NULL)
		return;
	RSA_free(k->rsa);
	free(k);
}

int
sgug_rsa_save(const sgug_rsa *k, const char *path)
{
	FILE *f;
	int fd, rc;

	/* Created 0600 up front rather than chmod'ed afterwards, so the private
	 * key is never briefly readable. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;

	f = fdopen(fd, "wb");
	if (f == NULL) {
		close(fd);
		return -1;
	}

	rc = PEM_write_RSAPrivateKey(f, k->rsa, NULL, NULL, 0, NULL, NULL);
	fclose(f);
	return rc == 1 ? 0 : -1;
}

sgug_rsa *
sgug_rsa_load(const char *path)
{
	struct stat st;
	sgug_rsa *k;
	FILE *f;

	if (stat(path, &st) != 0)
		return NULL;

	/* A key group or world can read is already compromised; failing loudly
	 * beats carrying on with it. */
	if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
		fprintf(stderr, "%s: permissions are %lo, want 0600\n",
		    path, (unsigned long)(st.st_mode & 07777));
		return NULL;
	}

	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;

	k = calloc(1, sizeof(*k));
	if (k == NULL) {
		fclose(f);
		return NULL;
	}

	k->rsa = PEM_read_RSAPrivateKey(f, NULL, NULL, NULL);
	fclose(f);

	if (k->rsa == NULL) {
		free(k);
		return NULL;
	}
	return k;
}

/* BN_bn2bin emits raw big-endian bytes with no leading zero, which is exactly
 * the wire form. Going through it keeps this independent of host endianness. */
static int
bn_to_b64(const BIGNUM *bn, char *out, size_t outlen)
{
	unsigned char buf[RSA_BITS / 8 + 8];
	int n;

	if (bn == NULL)
		return -1;

	n = BN_num_bytes(bn);
	if (n <= 0 || (size_t)n > sizeof(buf))
		return -1;
	if (BN_bn2bin(bn, buf) != n)
		return -1;

	return sgug_b64_encode(buf, (size_t)n, out, outlen);
}

int
sgug_rsa_public_modulus_b64(const sgug_rsa *k, char *out, size_t outlen)
{
	const BIGNUM *n, *e, *d;

	RSA_get0_key(k->rsa, &n, &e, &d);
	return bn_to_b64(n, out, outlen);
}

int
sgug_rsa_public_exponent_b64(const sgug_rsa *k, char *out, size_t outlen)
{
	const BIGNUM *n, *e, *d;

	RSA_get0_key(k->rsa, &n, &e, &d);
	return bn_to_b64(e, out, outlen);
}

int
sgug_rsa_sign_sha256(const sgug_rsa *k, const void *msg, size_t msglen,
    int use_pss, unsigned char *sig, size_t siglen)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	EVP_MD_CTX *mdctx = NULL;
	size_t n = siglen;
	int rc = -1;

	pkey = EVP_PKEY_new();
	if (pkey == NULL)
		goto out;

	/* Takes a reference on success, so the key outlives this call. */
	if (EVP_PKEY_set1_RSA(pkey, k->rsa) != 1)
		goto out;

	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL)
		goto out;

	if (EVP_DigestSignInit(mdctx, &pctx, EVP_sha256(), NULL, pkey) != 1)
		goto out;

	if (use_pss) {
		if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1)
			goto out;
		/* .NET's RSASignaturePadding.Pss uses a salt the length of the
		 * digest, which is not OpenSSL's default. */
		if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, 32) != 1)
			goto out;
	} else {
		if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) != 1)
			goto out;
	}

	if (EVP_DigestSign(mdctx, sig, &n, msg, msglen) != 1)
		goto out;

	rc = (int)n;

out:
	EVP_MD_CTX_free(mdctx);
	EVP_PKEY_free(pkey);
	return rc;
}

int
sgug_rsa_decrypt_oaep(const sgug_rsa *k, const unsigned char *ct,
    size_t ct_len, int use_sha256, unsigned char *out, size_t outlen)
{
	unsigned char buf[RSA_BITS / 8];
	int n;

	if (ct_len > sizeof(buf))
		return -1;

	/*
	 * RSA_private_decrypt only offers OAEP-SHA1. The SHA-256 variant needs
	 * the EVP interface to set the digest, so the two paths differ.
	 */
	if (!use_sha256) {
		n = RSA_private_decrypt((int)ct_len, ct, buf, k->rsa,
		    RSA_PKCS1_OAEP_PADDING);
		if (n < 0 || (size_t)n > outlen)
			return -1;
		memcpy(out, buf, (size_t)n);
		return n;
	} else {
		EVP_PKEY *pkey = NULL;
		EVP_PKEY_CTX *pctx = NULL;
		size_t len = outlen;
		int rc = -1;

		pkey = EVP_PKEY_new();
		if (pkey == NULL)
			goto out;
		if (EVP_PKEY_set1_RSA(pkey, k->rsa) != 1)
			goto out;

		pctx = EVP_PKEY_CTX_new(pkey, NULL);
		if (pctx == NULL || EVP_PKEY_decrypt_init(pctx) != 1)
			goto out;
		if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING) != 1)
			goto out;
		if (EVP_PKEY_CTX_set_rsa_oaep_md(pctx, EVP_sha256()) != 1)
			goto out;
		if (EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1)
			goto out;

		if (EVP_PKEY_decrypt(pctx, out, &len, ct, ct_len) != 1)
			goto out;

		rc = (int)len;

	out:
		EVP_PKEY_CTX_free(pctx);
		EVP_PKEY_free(pkey);
		return rc;
	}
}
