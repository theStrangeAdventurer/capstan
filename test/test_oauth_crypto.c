#include "munit.h"
#include "oauth_crypto.h"
#include <stdint.h>
#include <string.h>

/* Public interoperability fixture: RFC 7515 Appendix A.2 (no private key). */
static const char input[] = "eyJhbGciOiJSUzI1NiJ9."
    "eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
    "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ";
static const char modulus[] =
    "ofgWCuLjybRlzo0tZWJjNiuSfb4p4fAkd_wWJcyQoTbji9k0l8W26mPddx"
    "HmfHQp-Vaw-4qPCJrcS2mJPMEzP1Pt0Bm4d4QlL-yRT-SFd2lZS-pCgNMs"
    "D1W_YpRPEwOWvG6b32690r2jZ47soMZo9wGzjb_7OMg0LOL-bSf63kpaSH"
    "SXndS5z5rexMdbBYUsLA9e-KXBdQOS-UTo7WTBEMa2R2CapHg665xsmtdV"
    "MTBQY4uDZlxvb3qCo5ZwKh9kG4LT6_I5IhlJH7aGhyxXFvUK-DWNmoudF8"
    "NAco9_h9iaGNj8q2ethFkMLs91kzk2PAcDTW9gb54h4FRWyuXpoQ";
static const char signature[] =
    "cC4hiUPoj9Eetdgtv3hF80EGrhuB__dzERat0XF9g2VtQgr9PJbu3XOiZj5RZmh7"
    "AAuHIm4Bh-0Qc_lF5YKt_O8W2Fp5jujGbds9uJdbF9CUAr7t1dnZcAcQjbKBYNX4"
    "BAynRFdiuB--f_nZLgrnbyTyWzO75vRK5h6xBArLIARNPvkSjtQBMHlb1L07Qe7K"
    "0GarZRmB_eSN9383LcOLn6_dO--xi12jzDwusC-eOkHWEsqtFZESc6BfI7noOPqv"
    "hJ1phCnvWh6IeYI2w9QOYEUipUTI8np6LbgGY9Fs98rqVt5AXLIhWkWywlVmtVrB"
    "p0igcN_IoypGlUPQGe77Rw";

static MunitResult test_pkce(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    const char verifier[] = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    char out[44], bad[129];
    munit_assert_true(oauth_crypto_pkce_s256(verifier, strlen(verifier), out, sizeof(out)));
    munit_assert_string_equal(out, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    munit_assert_false(oauth_crypto_pkce_s256(verifier, 42, out, sizeof(out)));
    munit_assert_false(oauth_crypto_pkce_s256(verifier, 43, out, 43));
    memset(bad, 'a', sizeof(bad));
    munit_assert_false(oauth_crypto_pkce_s256(bad, 129, out, sizeof(out)));
    munit_assert_true(oauth_crypto_pkce_s256(bad, 128, out, sizeof(out)));
    bad[0] = '=';
    munit_assert_false(oauth_crypto_pkce_s256(bad, 43, out, sizeof(out)));
    return MUNIT_OK;
}

static MunitResult test_base64(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    unsigned char out[OAUTH_CRYPTO_MAX_BYTES], bytes[OAUTH_CRYPTO_MAX_BYTES];
    char encoded[OAUTH_CRYPTO_MAX_BASE64URL + 1];
    size_t len;
    const char *invalid[] = {"A", "AA=", "AA==", "AB", "AAB", "A A", "AA\n", "+w", "/w", "===="};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        munit_assert_false(oauth_crypto_base64url_decode(invalid[i], strlen(invalid[i]), out, sizeof(out), &len));
        munit_assert_size(len, ==, 0);
    }
    munit_assert_false(oauth_crypto_base64url_decode("AA\0A", 4, out, sizeof(out), &len));
    munit_assert_false(oauth_crypto_base64url_decode("AA", SIZE_MAX, out, sizeof(out), &len));
    munit_assert_false(oauth_crypto_base64url_decode("AA", 2, out, 0, &len));
    munit_assert_false(oauth_crypto_base64url_encode(bytes, SIZE_MAX, encoded, sizeof(encoded)));
    munit_assert_true(oauth_crypto_base64url_decode("", 0, out, sizeof(out), &len));
    munit_assert_size(len, ==, 0);
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)i;
    const size_t sizes[] = {0, 1, 2, 3, 32, sizeof(bytes)};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        munit_assert_true(oauth_crypto_base64url_encode(bytes, sizes[i], encoded, sizeof(encoded)));
        munit_assert_true(oauth_crypto_base64url_decode(encoded, strlen(encoded), out, sizeof(out), &len));
        munit_assert_size(len, ==, sizes[i]);
        munit_assert_memory_equal(len, bytes, out);
    }
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *wire[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        munit_assert_true(oauth_crypto_base64url_encode((const unsigned char *)plain[i], strlen(plain[i]), encoded, sizeof(encoded)));
        munit_assert_string_equal(encoded, wire[i]);
        munit_assert_true(oauth_crypto_base64url_decode(wire[i], strlen(wire[i]), out, sizeof(out), &len));
        munit_assert_size(len, ==, strlen(plain[i]));
        munit_assert_memory_equal(len, plain[i], out);
    }
    munit_assert_true(oauth_crypto_base64url_encode((const unsigned char *)"\xfb\xff", 2, encoded, sizeof(encoded)));
    munit_assert_string_equal(encoded, "-_8");
    return MUNIT_OK;
}

static int verify(const char *text, const char *sig, const char *n, const char *e) {
    return oauth_crypto_verify_rs256(text, strlen(text), sig, strlen(sig), n, strlen(n), e, strlen(e));
}

static MunitResult test_rs256(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    char altered_sig[sizeof(signature)], wrong_key[sizeof(modulus)];
    munit_assert_true(verify(input, signature, modulus, "AQAB"));
    memcpy(altered_sig, signature, sizeof(signature)); altered_sig[0] = 'd';
    munit_assert_false(verify(input, altered_sig, modulus, "AQAB"));
    memcpy(wrong_key, modulus, sizeof(modulus)); wrong_key[10] = 'A';
    munit_assert_false(verify(input, signature, wrong_key, "AQAB"));
    munit_assert_false(verify("altered", signature, modulus, "AQAB"));
    munit_assert_false(verify(input, signature + 1, modulus, "AQAB"));
    munit_assert_false(verify(input, signature, "AQAB", "AQAB"));
    munit_assert_false(verify(input, signature, modulus, "Ag"));
    munit_assert_false(verify(input, signature, modulus, "AQ"));
    munit_assert_false(verify(input, signature, modulus, "AAEAAQ"));
    munit_assert_false(verify(input, signature, modulus, "AQAB="));
    munit_assert_false(verify(input, signature, modulus, ""));
    unsigned char maximum[1024], zero_sig[1024] = {0};
    char large_n[1367], large_sig[1367];
    memset(maximum, 0xff, sizeof(maximum));
    munit_assert_true(oauth_crypto_base64url_encode(maximum, sizeof(maximum), large_n, sizeof(large_n)));
    munit_assert_true(oauth_crypto_base64url_encode(zero_sig, sizeof(zero_sig), large_sig, sizeof(large_sig)));
    munit_assert_false(verify(input, large_sig, large_n, "_____w"));
    maximum[0] = 0;
    munit_assert_true(oauth_crypto_base64url_encode(maximum, sizeof(maximum), large_n, sizeof(large_n)));
    munit_assert_false(verify(input, large_sig, large_n, "AQAB"));
    munit_assert_false(oauth_crypto_verify_rs256(input, SIZE_MAX, signature, strlen(signature), modulus, strlen(modulus), "AQAB", 4));
    return MUNIT_OK;
}

static MunitResult test_random(const MunitParameter params[], void *data) {
    (void)params; (void)data;
    unsigned char bytes[32];
    munit_assert_true(oauth_crypto_random(bytes, sizeof(bytes)));
    munit_assert_false(oauth_crypto_random(NULL, 32));
    munit_assert_false(oauth_crypto_random(bytes, 0));
    munit_assert_false(oauth_crypto_random(bytes, SIZE_MAX));
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/pkce", test_pkce, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/base64url", test_base64, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rs256", test_rs256, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/random", test_random, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite oauth_crypto_suite = {"/oauth_crypto", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};

#ifdef OAUTH_CRYPTO_TEST_MAIN
int main(int argc, char **argv) {
    return munit_suite_main(&oauth_crypto_suite, NULL, argc, argv);
}
#endif
