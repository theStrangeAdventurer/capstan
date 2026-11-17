#ifndef CAPSTAN_OAUTH_CRYPTO_H
#define CAPSTAN_OAUTH_CRYPTO_H

#include <stddef.h>

#define OAUTH_CRYPTO_MAX_BYTES 16384
#define OAUTH_CRYPTO_MAX_BASE64URL 21846

/* All functions return 1 on success, 0 on invalid input/crypto failure.
 * Strings use explicit lengths; encoded outputs are NUL terminated.
 * Decode accepts only canonical unpadded base64url, including empty input.
 * Output contents are unspecified on failure; do not consume them. */
int oauth_crypto_random(unsigned char *out, size_t len);
int oauth_crypto_base64url_encode(const unsigned char *in, size_t len,
                                  char *out, size_t capacity);
int oauth_crypto_base64url_decode(const char *in, size_t len,
                                  unsigned char *out, size_t capacity,
                                  size_t *out_len);
/* RFC 7636 verifier: 43..128 ASCII unreserved characters. Output needs 44 bytes. */
int oauth_crypto_pkce_s256(const char *verifier, size_t len,
                           char *out, size_t capacity);
/* RS256 ONLY, RSA 2048..8192 bits, exponent odd >=3 (at most 32 bits).
 * Signature and JWK n/e are canonical unpadded base64url strings.
 * No JWT parsing or claims validation. Input is exact ASCII JWT signing input. */
int oauth_crypto_verify_rs256(const char *input, size_t input_len,
                              const char *signature, size_t signature_len,
                              const char *n, size_t n_len,
                              const char *e, size_t e_len);

#endif
