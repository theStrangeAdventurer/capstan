#include "oauth_crypto.h"

#include <stdint.h>
#include <string.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#else
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#endif

int oauth_crypto_random(unsigned char *out, size_t len) {
    if (!out || !len || len > OAUTH_CRYPTO_MAX_BYTES) return 0;
#ifdef __APPLE__
    return SecRandomCopyBytes(kSecRandomDefault, len, out) == errSecSuccess;
#else
    return RAND_bytes(out, (int)len) == 1;
#endif
}

int oauth_crypto_base64url_encode(const unsigned char *in, size_t len,
                                  char *out, size_t capacity) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if ((!in && len) || !out || len > OAUTH_CRYPTO_MAX_BYTES) return 0;
    size_t needed = (len * 8 + 5) / 6;
    if (capacity <= needed) return 0;
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[pos++] = alphabet[(v >> 18) & 63];
        out[pos++] = alphabet[(v >> 12) & 63];
        if (i + 1 < len) out[pos++] = alphabet[(v >> 6) & 63];
        if (i + 2 < len) out[pos++] = alphabet[v & 63];
    }
    out[pos] = '\0';
    return 1;
}

int oauth_crypto_base64url_decode(const char *in, size_t len,
                                  unsigned char *out, size_t capacity,
                                  size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!in || !out || !out_len || len > OAUTH_CRYPTO_MAX_BASE64URL || len % 4 == 1)
        return 0;
    size_t needed = len * 6 / 8;
    if (needed > capacity || needed > OAUTH_CRYPTO_MAX_BYTES) return 0;
    unsigned value = 0, bits = 0;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        unsigned digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '-') digit = 62;
        else if (c == '_') digit = 63;
        else return 0;
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[pos++] = (unsigned char)(value >> bits);
            value &= (1u << bits) - 1;
        }
    }
    /* Unused low bits must be zero: reject non-canonical encodings. */
    if (value != 0) return 0;
    *out_len = pos;
    return 1;
}

int oauth_crypto_pkce_s256(const char *verifier, size_t len,
                           char *out, size_t capacity) {
    unsigned char digest[32];
    if (!verifier || len < 43 || len > 128) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)verifier[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~'))
            return 0;
    }
#ifdef __APPLE__
    if (!CC_SHA256(verifier, (CC_LONG)len, digest)) return 0;
#else
    unsigned int digest_len = 0;
    if (EVP_Digest(verifier, len, digest, &digest_len, EVP_sha256(), NULL) != 1 ||
        digest_len != sizeof(digest)) return 0;
#endif
    return oauth_crypto_base64url_encode(digest, sizeof(digest), out, capacity);
}

#ifdef __APPLE__
/* Encode the two bounded JWK integers as a PKCS#1 public key for SecKey.
 * This is serialization only; Security.framework owns all RSA operations. */
static size_t der_length(unsigned char *out, size_t length) {
    if (length < 128) { out[0] = (unsigned char)length; return 1; }
    if (length < 256) { out[0] = 0x81; out[1] = (unsigned char)length; return 2; }
    out[0] = 0x82; out[1] = (unsigned char)(length >> 8);
    out[2] = (unsigned char)length; return 3;
}

static size_t der_integer(unsigned char *out, const unsigned char *bytes, size_t length) {
    size_t padding = (bytes[0] & 0x80) != 0;
    out[0] = 0x02;
    size_t pos = 1 + der_length(out + 1, length + padding);
    if (padding) out[pos++] = 0;
    memcpy(out + pos, bytes, length);
    return pos + length;
}

static int verify_apple(const char *input, size_t input_len,
                        const unsigned char *sig, size_t sig_len,
                        const unsigned char *modulus, size_t modulus_len,
                        const unsigned char *exponent, size_t exponent_len) {
    unsigned char body[1040], der[1050];
    size_t body_len = der_integer(body, modulus, modulus_len);
    body_len += der_integer(body + body_len, exponent, exponent_len);
    der[0] = 0x30;
    size_t prefix = 1 + der_length(der + 1, body_len);
    memcpy(der + prefix, body, body_len);
    const void *names[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void *values[] = {kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, names, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef key_data = CFDataCreate(NULL, der, (CFIndex)(prefix + body_len));
    CFDataRef message = CFDataCreate(NULL, (const UInt8 *)input, (CFIndex)input_len);
    CFDataRef signature = CFDataCreate(NULL, sig, (CFIndex)sig_len);
    SecKeyRef key = attrs && key_data ? SecKeyCreateWithData(key_data, attrs, NULL) : NULL;
    int ok = key && message && signature &&
        SecKeyIsAlgorithmSupported(key, kSecKeyOperationTypeVerify,
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256) &&
        SecKeyVerifySignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
            message, signature, NULL);
    if (key) CFRelease(key);
    if (signature) CFRelease(signature);
    if (message) CFRelease(message);
    if (key_data) CFRelease(key_data);
    if (attrs) CFRelease(attrs);
    return ok;
}
#endif

int oauth_crypto_verify_rs256(const char *input, size_t input_len,
                              const char *signature, size_t signature_len,
                              const char *n, size_t n_len,
                              const char *e, size_t e_len) {
    unsigned char modulus[1024], exponent[4], sig[1024];
    size_t modulus_len = 0, exponent_len = 0, sig_len = 0;
    if (!input || !input_len || input_len > OAUTH_CRYPTO_MAX_BYTES ||
        !oauth_crypto_base64url_decode(n, n_len, modulus, sizeof(modulus), &modulus_len) ||
        !oauth_crypto_base64url_decode(e, e_len, exponent, sizeof(exponent), &exponent_len) ||
        !oauth_crypto_base64url_decode(signature, signature_len, sig, sizeof(sig), &sig_len) ||
        modulus_len < 256 || !exponent_len || !modulus[0] || !exponent[0] ||
        sig_len != modulus_len) return 0;
    uint32_t exponent_value = 0;
    for (size_t i = 0; i < exponent_len; i++) exponent_value = (exponent_value << 8) | exponent[i];
    if ((modulus_len == 256 && !(modulus[0] & 0x80)) ||
        !(modulus[modulus_len - 1] & 1) || !(exponent_value & 1) || exponent_value < 3) return 0;
#ifdef __APPLE__
    return verify_apple(input, input_len, sig, sig_len, modulus, modulus_len, exponent, exponent_len);
#else
    int ok = 0;
    BIGNUM *bn_n = NULL, *bn_e = NULL;
    OSSL_PARAM_BLD *builder = NULL;
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *key_ctx = NULL, *verify_ctx = NULL;
    EVP_PKEY *key = NULL;
    EVP_MD_CTX *md = NULL;
    bn_n = BN_bin2bn(modulus, (int)modulus_len, NULL);
    bn_e = BN_bin2bn(exponent, (int)exponent_len, NULL);
    if (!bn_n || !bn_e || BN_num_bits(bn_n) < 2048 || !BN_is_odd(bn_n) ||
        !BN_is_odd(bn_e) || BN_get_word(bn_e) < 3) goto done;
    builder = OSSL_PARAM_BLD_new();
    key_ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    md = EVP_MD_CTX_new();
    if (!builder || !key_ctx || !md ||
        !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, bn_n) ||
        !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, bn_e)) goto done;
    params = OSSL_PARAM_BLD_to_param(builder);
    if (!params || EVP_PKEY_fromdata_init(key_ctx) <= 0 ||
        EVP_PKEY_fromdata(key_ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0 ||
        EVP_DigestVerifyInit(md, &verify_ctx, EVP_sha256(), NULL, key) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(verify_ctx, RSA_PKCS1_PADDING) <= 0) goto done;
    ok = EVP_DigestVerify(md, sig, sig_len, (const unsigned char *)input, input_len) == 1;
done:
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(key_ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(builder);
    BN_free(bn_n);
    BN_free(bn_e);
    return ok;
#endif
}
