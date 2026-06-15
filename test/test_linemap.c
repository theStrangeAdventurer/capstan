#include "munit.h"
#include "linemap.h"
#include <string.h>

static MunitResult test_empty_messages(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    linemap_build(NULL, NULL, 0, NULL, 80);
    munit_assert_int(linemap_count(), ==, 0);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_single_line(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"hello"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 80);
    munit_assert_int(linemap_count(), ==, 1);
    const LineInfo *li = linemap_get(0);
    munit_assert_not_null(li);
    munit_assert_int(li->byte_start, ==, 0);
    munit_assert_int(li->byte_end, ==, 5);
    munit_assert_int(li->char_count, ==, 5);
    munit_assert_int(li->role, ==, 0);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_multiline_newline(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"hello\nworld"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 80);
    munit_assert_int(linemap_count(), ==, 2);
    const LineInfo *l0 = linemap_get(0);
    munit_assert_int(l0->byte_start, ==, 0);
    munit_assert_int(l0->byte_end, ==, 5);
    munit_assert_int(l0->char_count, ==, 5);
    const LineInfo *l1 = linemap_get(1);
    munit_assert_int(l1->byte_start, ==, 6);
    munit_assert_int(l1->byte_end, ==, 11);
    munit_assert_int(l1->char_count, ==, 5);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_word_wrap(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"abcdef"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 3);
    munit_assert_int(linemap_count(), ==, 2);
    const LineInfo *l0 = linemap_get(0);
    munit_assert_int(l0->char_count, ==, 3);
    const LineInfo *l1 = linemap_get(1);
    munit_assert_int(l1->char_count, ==, 3);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_empty_text_message(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {""};
    int roles[] = {1};
    linemap_build(NULL, roles, 1, texts, 80);
    munit_assert_int(linemap_count(), ==, 1);
    const LineInfo *li = linemap_get(0);
    munit_assert_int(li->char_count, ==, 0);
    munit_assert_int(li->role, ==, 1);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_multiple_messages(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"hi", "bye"};
    int roles[] = {0, 1};
    linemap_build(NULL, roles, 2, texts, 80);
    munit_assert_int(linemap_count(), ==, 2);
    munit_assert_int(linemap_get(0)->msg_index, ==, 0);
    munit_assert_int(linemap_get(1)->msg_index, ==, 1);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_utf8_chars(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"привет"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 3);
    munit_assert_int(linemap_count(), ==, 2);
    const LineInfo *l0 = linemap_get(0);
    munit_assert_int(l0->char_count, ==, 3);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_wrap_with_newline(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"abc\ndefgh"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 3);
    munit_assert_int(linemap_count(), ==, 3);
    munit_assert_int(linemap_get(0)->char_count, ==, 3);
    munit_assert_int(linemap_get(1)->char_count, ==, 3);
    munit_assert_int(linemap_get(2)->char_count, ==, 2);
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_get_out_of_bounds(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"x"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 80);
    munit_assert_null(linemap_get(-1));
    munit_assert_null(linemap_get(1));
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_long_wrap(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    const char *texts[] = {"abcdefghij"};
    int roles[] = {0};
    linemap_build(NULL, roles, 1, texts, 4);
    munit_assert_int(linemap_count(), ==, 3);
    munit_assert_int(linemap_get(0)->char_count, ==, 4);
    munit_assert_int(linemap_get(1)->char_count, ==, 4);
    munit_assert_int(linemap_get(2)->char_count, ==, 2);
    linemap_free();
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/empty_messages", test_empty_messages, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/single_line", test_single_line, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/multiline_newline", test_multiline_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_wrap", test_word_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/empty_text", test_empty_text_message, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/multiple_messages", test_multiple_messages, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/utf8", test_utf8_chars, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/wrap_newline", test_wrap_with_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/out_of_bounds", test_get_out_of_bounds, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/long_wrap", test_long_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite linemap_suite = {"/linemap", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
