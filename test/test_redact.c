#include "munit.h"
#include "redact.h"
#include <stdlib.h>
#include <string.h>

static MunitResult test_redacts_sensitive_headers(const MunitParameter params[],
                                                  void *data) {
  (void)params;
  (void)data;
  char *out = redact_secrets_alloc("Authorization: Bearer secret-token\nOK: yes");
  munit_assert_not_null(out);
  munit_assert_null(strstr(out, "secret-token"));
  munit_assert_not_null(strstr(out, "Authorization: [REDACTED]"));
  munit_assert_not_null(strstr(out, "OK: yes"));
  free(out);
  return MUNIT_OK;
}

static MunitResult test_redacts_key_values(const MunitParameter params[],
                                           void *data) {
  (void)params;
  (void)data;
  char *out = redact_secrets_alloc(
      "{\"access_token\":\"json-secret\",\"password\":\"pw\",\"passwd\":\"short\"} "
      "token=abc");
  munit_assert_not_null(out);
  munit_assert_null(strstr(out, "json-secret"));
  munit_assert_null(strstr(out, "\"pw\""));
  munit_assert_null(strstr(out, "\"short\""));
  munit_assert_null(strstr(out, "abc"));
  munit_assert_not_null(strstr(out, "\"access_token\":\"[REDACTED]\""));
  munit_assert_not_null(strstr(out, "\"password\":\"[REDACTED]\""));
  munit_assert_not_null(strstr(out, "\"passwd\":\"[REDACTED]\""));
  free(out);
  return MUNIT_OK;
}

static MunitResult test_redacts_env_secrets_but_not_path(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *out = redact_secrets_alloc(
      "PATH=/usr/bin OPENAI_API_KEY=sk-secret NORMAL=value AUTH_TOKEN=tok");
  munit_assert_not_null(out);
  munit_assert_not_null(strstr(out, "PATH=/usr/bin"));
  munit_assert_not_null(strstr(out, "NORMAL=value"));
  munit_assert_null(strstr(out, "sk-secret"));
  munit_assert_null(strstr(out, "AUTH_TOKEN=tok"));
  free(out);
  return MUNIT_OK;
}

static MunitResult test_redacts_sensitive_key_contexts(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *out = redact_secrets_alloc(
      "OPENAI_API_KEY=api-secret\nx-subscription-token: header-secret\n"
      "private_key=priv sort_key=created_at cache_key=model-list");
  munit_assert_not_null(out);
  munit_assert_null(strstr(out, "api-secret"));
  munit_assert_null(strstr(out, "header-secret"));
  munit_assert_not_null(strstr(out, "private_key=priv"));
  munit_assert_not_null(strstr(out, "sort_key=created_at"));
  munit_assert_not_null(strstr(out, "cache_key=model-list"));
  free(out);
  return MUNIT_OK;
}

static MunitResult test_redacts_subscription_token_headers(
    const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  char *out = redact_secrets_alloc(
      "x-subscription-token: sub-secret\nxsubscription.token: dot-secret");
  munit_assert_not_null(out);
  munit_assert_null(strstr(out, "sub-secret"));
  munit_assert_null(strstr(out, "dot-secret"));
  munit_assert_not_null(strstr(out, "x-subscription-token: [REDACTED]"));
  munit_assert_not_null(strstr(out, "xsubscription.token: [REDACTED]"));
  free(out);
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/redacts_sensitive_headers", test_redacts_sensitive_headers, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/redacts_key_values", test_redacts_key_values, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/redacts_env_secrets_but_not_path",
     test_redacts_env_secrets_but_not_path, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/redacts_sensitive_key_contexts", test_redacts_sensitive_key_contexts,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/redacts_subscription_token_headers",
     test_redacts_subscription_token_headers, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite redact_suite = {"/redact", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
