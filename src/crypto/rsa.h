#ifndef SGUG_CRYPTO_RSA_H
#define SGUG_CRYPTO_RSA_H

#include <stddef.h>

typedef struct sgug_rsa sgug_rsa;

/* Generates a 2048-bit key. The service rejects smaller. */
sgug_rsa *sgug_rsa_generate(void);
void sgug_rsa_free(sgug_rsa *k);

/*
 * Persistence is PKCS#1 DER, not the reference runner's
 * .credentials_rsaparams. Nothing on the wire depends on the file format, and
 * a Newtonsoft-serialised blob of base64 fields buys nothing here.
 * Written 0600; refuses to load a file with looser permissions.
 */
int sgug_rsa_save(const sgug_rsa *k, const char *path);
sgug_rsa *sgug_rsa_load(const char *path);

/*
 * Public key in the shape the agent registration expects: standard base64 of
 * the raw big-endian integer bytes, with no leading zero byte and no DER
 * wrapper. The exponent is normally "AQAB".
 *
 * This is the field most likely to break on a big-endian host if it were
 * built by hand, which is why it goes through BN_bn2bin rather than any
 * in-memory representation.
 */
int sgug_rsa_public_modulus_b64(const sgug_rsa *k, char *out, size_t outlen);
int sgug_rsa_public_exponent_b64(const sgug_rsa *k, char *out, size_t outlen);

/*
 * Signs with SHA-256. use_pss selects PS256 over RS256, which the service
 * requires when the agent's properties carry RequireFipsCryptography, and
 * which is the common case on github.com today.
 *
 * Returns signature length, or -1. A 2048-bit key produces 256 bytes.
 */
int sgug_rsa_sign_sha256(const sgug_rsa *k, const void *msg, size_t msglen,
    int use_pss, unsigned char *sig, size_t siglen);

/*
 * Recovers the session key from TaskAgentSession.encryptionKey.value.
 *
 * RSA-OAEP, not PKCS#1 v1.5. use_sha256 follows the session's
 * useFipsEncryption; we send false, so this is normally OAEP-SHA1. The label is
 * empty. Yields 32 bytes for AES-256.
 */
int sgug_rsa_decrypt_oaep(const sgug_rsa *k, const unsigned char *ct,
    size_t ct_len, int use_sha256, unsigned char *out, size_t outlen);

#endif /* SGUG_CRYPTO_RSA_H */
