/*
 * Vectors are from RFC 4648 and NIST SP 800-38A where published, and otherwise
 * round-trips. Running the same binary on x86 and on MIPS is the point: a
 * divergence localises a byte-order bug to the function that disagreed.
 */

#include "crypto/aes.h"
#include "crypto/b64.h"
#include "crypto/jwt.h"
#include "crypto/rsa.h"

#include <stdio.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_STR(got, want) \
	do { \
		const char *g = (got); \
		if (strcmp(g, (want)) != 0) { \
			printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, #got, g, (want)); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_INT(got, want) \
	do { \
		long g = (long)(got); \
		long w = (long)(want); \
		if (g != w) { \
			printf("FAIL %s:%d: %s = %ld, want %ld\n", \
			    __FILE__, __LINE__, #got, g, w); \
			failures++; \
		} \
	} while (0)

/* RFC 4648 section 10. */
static void
test_b64_vectors(void)
{
	static const struct {
		const char *in;
		const char *out;
	} V[] = {
		{ "", "" },
		{ "f", "Zg==" },
		{ "fo", "Zm8=" },
		{ "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" },
		{ "fooba", "Zm9vYmE=" },
		{ "foobar", "Zm9vYmFy" }
	};
	char buf[64];
	unsigned char back[64];
	size_t i;

	for (i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
		int n = sgug_b64_encode(V[i].in, strlen(V[i].in), buf, sizeof(buf));

		CHECK(n >= 0);
		CHECK_EQ_STR(buf, V[i].out);

		n = sgug_b64_decode(V[i].out, back, sizeof(back));
		CHECK_EQ_INT(n, (long)strlen(V[i].in));
		CHECK(memcmp(back, V[i].in, strlen(V[i].in)) == 0);
	}
}

static void
test_b64url(void)
{
	/* These bytes exercise both characters the alphabets differ on: the
	 * standard form is "++/+" versus url-safe "--_-". */
	static const unsigned char raw[] = { 0xfb, 0xef, 0xfe, 0xfb };
	char std[16], url[16];

	CHECK(sgug_b64_encode(raw, sizeof(raw), std, sizeof(std)) == 8);
	CHECK_EQ_STR(std, "++/++w==");

	/* URL-safe output is unpadded, which every JWT segment requires. */
	CHECK(sgug_b64url_encode(raw, sizeof(raw), url, sizeof(url)) == 6);
	CHECK_EQ_STR(url, "--_--w");

	CHECK(strchr(url, '=') == NULL);
}

static void
test_b64_rejects_bad_input(void)
{
	unsigned char out[32];

	CHECK(sgug_b64_decode("Zm9v!", out, sizeof(out)) == -1);
	/* A lone trailing sextet cannot come from whole bytes. */
	CHECK(sgug_b64_decode("Zm9vY", out, sizeof(out)) == -1);
	/* Non-canonical: the unused low bits are not zero. */
	CHECK(sgug_b64_decode("Zg==", out, sizeof(out)) == 1);
	CHECK(sgug_b64_decode("Zh==", out, sizeof(out)) == -1);
	/* Output buffer too small must fail rather than truncate. */
	CHECK(sgug_b64_decode("Zm9vYmFy", out, 2) == -1);
}

static void
test_b64_binary_roundtrip(void)
{
	unsigned char raw[256], back[256];
	char enc[512];
	int i, n;

	for (i = 0; i < 256; i++)
		raw[i] = (unsigned char)i;

	n = sgug_b64_encode(raw, sizeof(raw), enc, sizeof(enc));
	CHECK(n > 0);

	n = sgug_b64_decode(enc, back, sizeof(back));
	CHECK_EQ_INT(n, 256);
	CHECK(memcmp(raw, back, sizeof(raw)) == 0);
}

/* Builds a real ciphertext with EVP so the test exercises our unpadding and
 * BOM handling rather than a hand-rolled fixture. */
static int
encrypt_for_test(const unsigned char *key, const unsigned char *iv,
    const void *pt, int ptlen, unsigned char *ct, int ctcap)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int n, total = 0;

	(void)ctcap;
	EVP_EncryptInit_ex(c, EVP_aes_256_cbc(), NULL, key, iv);
	EVP_EncryptUpdate(c, ct, &n, pt, ptlen);
	total = n;
	EVP_EncryptFinal_ex(c, ct + total, &n);
	total += n;
	EVP_CIPHER_CTX_free(c);
	return total;
}

static void
test_aes_decrypt(void)
{
	unsigned char key[32], iv[16], ct[256];
	unsigned char withbom[128];
	char out[256];
	const char *json = "{\"messageType\":\"PipelineAgentJobRequest\"}";
	int ctlen, n;
	size_t jlen = strlen(json);

	memset(key, 0x2b, sizeof(key));
	memset(iv, 0x1c, sizeof(iv));

	ctlen = encrypt_for_test(key, iv, json, (int)jlen, ct, sizeof(ct));
	n = sgug_aes_cbc_decrypt(key, iv, ct, (size_t)ctlen, out, sizeof(out));
	CHECK_EQ_INT(n, (long)jlen);
	CHECK_EQ_STR(out, json);

	/* The reference runner writes the plaintext through a .NET StreamWriter,
	 * which emits a UTF-8 BOM. It must be stripped or JSON parsing fails on
	 * the first token. */
	withbom[0] = 0xef;
	withbom[1] = 0xbb;
	withbom[2] = 0xbf;
	memcpy(withbom + 3, json, jlen);

	ctlen = encrypt_for_test(key, iv, withbom, (int)(jlen + 3), ct, sizeof(ct));
	n = sgug_aes_cbc_decrypt(key, iv, ct, (size_t)ctlen, out, sizeof(out));
	CHECK_EQ_INT(n, (long)jlen);
	CHECK_EQ_STR(out, json);
}

static void
test_aes_block_aligned_padding(void)
{
	unsigned char key[32], iv[16], ct[128];
	char out[128];
	/* Exactly 32 bytes, so PKCS#7 appends a whole block of 0x10. Treating
	 * the last byte as a count without validating the rest would silently
	 * corrupt a message that genuinely ended in bytes valued 1 to 16. */
	const char *pt = "0123456789abcdef0123456789abcdef";
	int ctlen, n;

	memset(key, 0x7f, sizeof(key));
	memset(iv, 0x03, sizeof(iv));

	ctlen = encrypt_for_test(key, iv, pt, 32, ct, sizeof(ct));
	CHECK_EQ_INT(ctlen, 48);

	n = sgug_aes_cbc_decrypt(key, iv, ct, (size_t)ctlen, out, sizeof(out));
	CHECK_EQ_INT(n, 32);
	CHECK_EQ_STR(out, pt);
}

static void
test_aes_rejects_bad_input(void)
{
	unsigned char key[32], iv[16], ct[64];
	char out[128];

	memset(key, 0x11, sizeof(key));
	memset(iv, 0x22, sizeof(iv));
	memset(ct, 0x33, sizeof(ct));

	/* Not a multiple of the block size. */
	CHECK(sgug_aes_cbc_decrypt(key, iv, ct, 33, out, sizeof(out)) == -1);
	CHECK(sgug_aes_cbc_decrypt(key, iv, ct, 0, out, sizeof(out)) == -1);
	/* Output buffer must have room for the plaintext plus a terminator. */
	CHECK(sgug_aes_cbc_decrypt(key, iv, ct, 32, out, 16) == -1);
}

static void
test_rsa_public_encoding(void)
{
	sgug_rsa *k = sgug_rsa_generate();
	char mod[512], exp[32];
	unsigned char raw[512];
	int n;

	CHECK(k != NULL);
	if (k == NULL)
		return;

	CHECK(sgug_rsa_public_exponent_b64(k, exp, sizeof(exp)) > 0);
	/* RSA_F4 is 65537, whose three big-endian bytes 01 00 01 encode as
	 * AQAB. A wrong-endian encoder would produce AQAB reversed. */
	CHECK_EQ_STR(exp, "AQAB");

	CHECK(sgug_rsa_public_modulus_b64(k, mod, sizeof(mod)) > 0);
	n = sgug_b64_decode(mod, raw, sizeof(raw));
	CHECK_EQ_INT(n, 256);
	/* Raw big-endian with no leading zero, so the top bit of a 2048-bit
	 * modulus is always set. */
	CHECK((raw[0] & 0x80) != 0);

	sgug_rsa_free(k);
}

static void
test_rsa_sign(void)
{
	sgug_rsa *k = sgug_rsa_generate();
	unsigned char sig[512];
	const char *msg = "eyJhbGciOiJSUzI1NiJ9.eyJpc3MiOiJ4In0";
	int n;

	CHECK(k != NULL);
	if (k == NULL)
		return;

	n = sgug_rsa_sign_sha256(k, msg, strlen(msg), 0, sig, sizeof(sig));
	CHECK_EQ_INT(n, 256);

	/* RS256 is deterministic, so two signings must agree. PS256 is
	 * randomised and must not. */
	{
		unsigned char again[512];

		CHECK(sgug_rsa_sign_sha256(k, msg, strlen(msg), 0, again,
		    sizeof(again)) == 256);
		CHECK(memcmp(sig, again, 256) == 0);

		CHECK(sgug_rsa_sign_sha256(k, msg, strlen(msg), 1, sig,
		    sizeof(sig)) == 256);
		CHECK(sgug_rsa_sign_sha256(k, msg, strlen(msg), 1, again,
		    sizeof(again)) == 256);
		CHECK(memcmp(sig, again, 256) != 0);
	}

	sgug_rsa_free(k);
}

/*
 * Rebuilds the public key from the base64 modulus and exponent we put on the
 * wire, encrypts to it, and decrypts with the private key. This covers the
 * whole path the service uses: if our wire encoding were byte-swapped, the
 * reconstructed key would differ and the round-trip would fail. A test that
 * reached into sgug_rsa directly would not catch that.
 */
static void
test_rsa_oaep_roundtrip(void)
{
	sgug_rsa *k = sgug_rsa_generate();
	unsigned char session_key[32], ct[256], out[64];
	unsigned char modraw[512], expraw[16];
	char mod[512], exp[32];
	RSA *pub = NULL;
	BIGNUM *n_bn, *e_bn;
	int i, modlen, explen, ctlen, n;

	CHECK(k != NULL);
	if (k == NULL)
		return;

	for (i = 0; i < 32; i++)
		session_key[i] = (unsigned char)(i * 7 + 1);

	CHECK(sgug_rsa_public_modulus_b64(k, mod, sizeof(mod)) > 0);
	CHECK(sgug_rsa_public_exponent_b64(k, exp, sizeof(exp)) > 0);

	modlen = sgug_b64_decode(mod, modraw, sizeof(modraw));
	explen = sgug_b64_decode(exp, expraw, sizeof(expraw));
	CHECK(modlen > 0);
	CHECK(explen > 0);

	n_bn = BN_bin2bn(modraw, modlen, NULL);
	e_bn = BN_bin2bn(expraw, explen, NULL);
	pub = RSA_new();
	CHECK(pub != NULL && n_bn != NULL && e_bn != NULL);
	if (pub == NULL || n_bn == NULL || e_bn == NULL) {
		sgug_rsa_free(k);
		return;
	}
	/* Takes ownership of both BIGNUMs. */
	RSA_set0_key(pub, n_bn, e_bn, NULL);

	ctlen = RSA_public_encrypt((int)sizeof(session_key), session_key, ct,
	    pub, RSA_PKCS1_OAEP_PADDING);
	CHECK_EQ_INT(ctlen, 256);

	n = sgug_rsa_decrypt_oaep(k, ct, (size_t)ctlen, 0, out, sizeof(out));
	CHECK_EQ_INT(n, 32);
	CHECK(n == 32 && memcmp(out, session_key, 32) == 0);

	/* Wrong OAEP digest must fail rather than return garbage. */
	CHECK(sgug_rsa_decrypt_oaep(k, ct, (size_t)ctlen, 1, out, sizeof(out)) < 0);

	/*
	 * The SHA-256 variant, which is what github.com actually uses to wrap
	 * the session key even when useFipsEncryption is false. Encrypt through
	 * EVP so the test drives the same digest the service does.
	 */
	{
		EVP_PKEY *pk = EVP_PKEY_new();
		EVP_PKEY_CTX *ec;
		size_t clen = sizeof(ct);

		CHECK(pk != NULL && EVP_PKEY_set1_RSA(pk, pub) == 1);
		ec = EVP_PKEY_CTX_new(pk, NULL);
		CHECK(ec != NULL);
		CHECK(EVP_PKEY_encrypt_init(ec) == 1);
		CHECK(EVP_PKEY_CTX_set_rsa_padding(ec, RSA_PKCS1_OAEP_PADDING) == 1);
		CHECK(EVP_PKEY_CTX_set_rsa_oaep_md(ec, EVP_sha256()) == 1);
		CHECK(EVP_PKEY_CTX_set_rsa_mgf1_md(ec, EVP_sha256()) == 1);
		CHECK(EVP_PKEY_encrypt(ec, ct, &clen, session_key,
		    sizeof(session_key)) == 1);
		CHECK_EQ_INT((long)clen, 256);

		n = sgug_rsa_decrypt_oaep(k, ct, clen, 1, out, sizeof(out));
		CHECK_EQ_INT(n, 32);
		CHECK(n == 32 && memcmp(out, session_key, 32) == 0);

		EVP_PKEY_CTX_free(ec);
		EVP_PKEY_free(pk);
	}

	RSA_free(pub);
	sgug_rsa_free(k);
}

static void
test_jwt_structure(void)
{
	sgug_rsa *k = sgug_rsa_generate();
	char tok[1400];
	char seg[1024];
	unsigned char dec[1024];
	const char *d1, *d2;
	int n;

	CHECK(k != NULL);
	if (k == NULL)
		return;

	n = sgug_jwt_client_assertion(k, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
	    "https://vstoken.actions.githubusercontent.com/_apis/oauth2/token",
	    1785118381, 0, tok, sizeof(tok));
	CHECK(n > 0);
	if (n <= 0) {
		sgug_rsa_free(k);
		return;
	}

	/* Exactly two dots, and no base64 padding anywhere. */
	d1 = strchr(tok, '.');
	CHECK(d1 != NULL);
	d2 = d1 != NULL ? strchr(d1 + 1, '.') : NULL;
	CHECK(d2 != NULL);
	CHECK(d2 != NULL && strchr(d2 + 1, '.') == NULL);
	CHECK(strchr(tok, '=') == NULL);
	CHECK(strchr(tok, '+') == NULL);
	CHECK(strchr(tok, '/') == NULL);

	if (d1 == NULL || d2 == NULL) {
		sgug_rsa_free(k);
		return;
	}

	memcpy(seg, tok, (size_t)(d1 - tok));
	seg[d1 - tok] = '\0';
	n = sgug_b64_decode(seg, dec, sizeof(dec) - 1);
	CHECK(n > 0);
	dec[n > 0 ? n : 0] = '\0';
	CHECK_EQ_STR((char *)dec, "{\"typ\":\"JWT\",\"alg\":\"RS256\"}");

	memcpy(seg, d1 + 1, (size_t)(d2 - d1 - 1));
	seg[d2 - d1 - 1] = '\0';
	n = sgug_b64_decode(seg, dec, sizeof(dec) - 1);
	CHECK(n > 0);
	dec[n > 0 ? n : 0] = '\0';

	/*
	 * nbf is backdated 30s for skew, and exp is nbf plus exactly 300s. The
	 * service measures lifetime as exp minus nbf and rejects anything over
	 * five minutes with a bare invalid_client, so the window must be 300
	 * and not 330.
	 */
	CHECK(strstr((char *)dec, "\"nbf\":1785118351") != NULL);
	CHECK(strstr((char *)dec, "\"exp\":1785118651") != NULL);
	CHECK(strstr((char *)dec, "\"iss\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"") != NULL);
	CHECK(strstr((char *)dec, "\"sub\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"") != NULL);
	/* No kid and no x5t: the service keys off client_id. */
	CHECK(strstr((char *)dec, "kid") == NULL);

	/* PS256 must be reflected in the header, not just the signature. */
	n = sgug_jwt_client_assertion(k, "x", "y", 1785118381, 1, tok, sizeof(tok));
	CHECK(n > 0);
	d1 = strchr(tok, '.');
	if (d1 != NULL) {
		memcpy(seg, tok, (size_t)(d1 - tok));
		seg[d1 - tok] = '\0';
		n = sgug_b64_decode(seg, dec, sizeof(dec) - 1);
		dec[n > 0 ? n : 0] = '\0';
		CHECK_EQ_STR((char *)dec, "{\"typ\":\"JWT\",\"alg\":\"PS256\"}");
	}

	sgug_rsa_free(k);
}

int
main(void)
{
	test_b64_vectors();
	test_b64url();
	test_b64_rejects_bad_input();
	test_b64_binary_roundtrip();
	test_aes_decrypt();
	test_aes_block_aligned_padding();
	test_aes_rejects_bad_input();
	test_rsa_public_encoding();
	test_rsa_sign();
	test_rsa_oaep_roundtrip();
	test_jwt_structure();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all crypto tests passed\n");
	return 0;
}
