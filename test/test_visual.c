#include "munit.h"
#include "linemap.h"
#include "visual.h"
#include <stdlib.h>
#include <string.h>

static const char *g_test_texts[] = {"hello world", "foo bar baz", "short"};
static int g_test_roles[] = {0, 1, 0};
static const int g_test_count = 3;

static void *setup(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    linemap_build(NULL, g_test_roles, g_test_count, g_test_texts, 80);
    const char **texts_copy = malloc(g_test_count * sizeof(const char *));
    for (int i = 0; i < g_test_count; i++)
        texts_copy[i] = g_test_texts[i];
    visual_set_texts(texts_copy, g_test_count);
    visual_enter();
    return NULL;
}

static void teardown(void *data) {
    (void)data;
    visual_exit();
    linemap_free();
}

static MunitResult test_enter_sets_active(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    munit_assert_true(visual_cursor_visible());
    munit_assert_false(visual_is_active());
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_exit_clears(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_exit();
    munit_assert_false(visual_cursor_visible());
    munit_assert_false(visual_is_active());
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_enter_cursor_at_last_line(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int line, col;
    visual_get_cursor(&line, &col);
    munit_assert_int(line, ==, linemap_count() - 1);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_move_up(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_move_up();
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, linemap_count() - 2);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_move_up_boundary(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int count = linemap_count();
    for (int i = 0; i < count + 5; i++)
        visual_move_up();
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_move_down_boundary(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int count = linemap_count();
    for (int i = 0; i < count + 5; i++)
        visual_move_down();
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, count - 1);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_move_left_right(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int col;
    visual_get_cursor(NULL, &col);
    int orig_col = col;
    visual_move_left();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, orig_col - 1);
    visual_move_right();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, orig_col);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_move_left_boundary(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int col;
    visual_get_cursor(NULL, &col);
    for (int i = 0; i < col + 5; i++)
        visual_move_left();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    visual_move_left();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_enter_selection(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_move_up();
    visual_enter_selection();
    munit_assert_true(visual_is_active());
    int sl, sc, el, ec;
    visual_selection_range(&sl, &sc, &el, &ec);
    munit_assert_int(el, ==, sl);
    munit_assert_int(ec, ==, sc);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_exit_selection(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_enter_selection();
    munit_assert_true(visual_is_active());
    visual_exit_selection();
    munit_assert_false(visual_is_active());
    munit_assert_true(visual_cursor_visible());
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_selection_range_forward(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int anchor_line;
    visual_get_cursor(&anchor_line, NULL);
    visual_enter_selection();
    visual_move_up();
    int cursor_line;
    visual_get_cursor(&cursor_line, NULL);
    int sl, sc, el, ec;
    visual_selection_range(&sl, &sc, &el, &ec);
    munit_assert_int(sl, ==, cursor_line);
    munit_assert_int(el, ==, anchor_line);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_selection_range_backward(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_move_up();
    int anchor_line;
    visual_get_cursor(&anchor_line, NULL);
    visual_enter_selection();
    visual_move_down();
    int cursor_line;
    visual_get_cursor(&cursor_line, NULL);
    int sl, sc, el, ec;
    visual_selection_range(&sl, &sc, &el, &ec);
    munit_assert_int(sl, ==, anchor_line);
    munit_assert_int(el, ==, cursor_line);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_set_cursor_line_clamp(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_set_cursor_line(-5);
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 0);
    visual_set_cursor_line(9999);
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, linemap_count() - 1);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_get_cursor_null_col(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, >=, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_empty_linemap(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    linemap_build(NULL, NULL, 0, NULL, 80);
    visual_enter();
    munit_assert_true(visual_cursor_visible());
    int line, col;
    visual_get_cursor(&line, &col);
    munit_assert_int(line, ==, 0);
    visual_exit();
    linemap_free();
    return MUNIT_OK;
}

static MunitResult test_line_start(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_move_left();
    visual_move_left();
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, >, 0);
    visual_move_line_start();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_line_end(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_move_line_start();
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    visual_move_line_end();
    visual_get_cursor(NULL, &col);
    const LineInfo *li = linemap_get(2);
    munit_assert_int(col, ==, li->char_count - 1);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_forward(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_set_cursor_line(0);
    visual_move_line_start();
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    visual_move_word_forward();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 6);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_forward_wrap(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_set_cursor_line(0);
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 0);
    const LineInfo *li = linemap_get(0);
    visual_move_line_end();
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, li->char_count - 1);
    visual_move_word_forward();
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 1);
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_backward(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_set_cursor_line(1);
    const LineInfo *li = linemap_get(1);
    visual_move_line_end();
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, li->char_count - 1);
    visual_move_word_backward();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 8);
    visual_move_word_backward();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 4);
    visual_move_word_backward();
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_backward_wrap(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    visual_set_cursor_line(1);
    visual_move_line_start();
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 1);
    visual_move_word_backward();
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, 0);
    int col;
    visual_get_cursor(NULL, &col);
    munit_assert_int(col, ==, 6);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_forward_boundary(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    for (int i = 0; i < 10; i++)
        visual_move_word_forward();
    int line;
    visual_get_cursor(&line, NULL);
    munit_assert_int(line, ==, linemap_count() - 1);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitResult test_word_backward_boundary(const MunitParameter params[], void *data) {
    (void)params;
    (void)data;
    setup(NULL, NULL);
    for (int i = 0; i < 10; i++)
        visual_move_word_backward();
    int line, col;
    visual_get_cursor(&line, &col);
    munit_assert_int(line, ==, 0);
    munit_assert_int(col, ==, 0);
    teardown(NULL);
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/enter_active", test_enter_sets_active, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/exit_clears", test_exit_clears, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/cursor_last", test_enter_cursor_at_last_line, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/move_up", test_move_up, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/move_up_boundary", test_move_up_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/move_down_boundary", test_move_down_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/move_left_right", test_move_left_right, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/move_left_boundary", test_move_left_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/enter_selection", test_enter_selection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/exit_selection", test_exit_selection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/range_forward", test_selection_range_forward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/range_backward", test_selection_range_backward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/set_cursor_clamp", test_set_cursor_line_clamp, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/get_null_col", test_get_cursor_null_col, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/empty_linemap", test_empty_linemap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/line_start", test_line_start, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/line_end", test_line_end, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_forward", test_word_forward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_forward_wrap", test_word_forward_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_backward", test_word_backward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_backward_wrap", test_word_backward_wrap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_forward_boundary", test_word_forward_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/word_backward_boundary", test_word_backward_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite visual_suite = {"/visual", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
