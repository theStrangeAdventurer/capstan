#include "munit.h"
#include "linemap.h"
#include "links.h"
#include "text_layout.h"
#include <string.h>

static MunitResult test_scans_urls(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *text = "see https://example.com/a?b=1 and http://x.dev.";
  LinkSpan spans[8];
  int count = links_scan(text, strlen(text), spans, 8);
  munit_assert_int(count, ==, 2);
  munit_assert_int(spans[0].kind, ==, LINK_URL);
  munit_assert_int((int)spans[0].start, ==, 4);
  munit_assert_int((int)(spans[0].end - spans[0].start), ==,
                   (int)strlen("https://example.com/a?b=1"));
  munit_assert_int(spans[1].kind, ==, LINK_URL);
  /* The sentence dot is not part of the link. */
  munit_assert_int((int)(spans[1].end - spans[1].start), ==,
                   (int)strlen("http://x.dev"));
  return MUNIT_OK;
}

static MunitResult test_scans_paths(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *text = "open /usr/local/bin/tool, src/tui.c and ~/notes/a.md";
  LinkSpan spans[8];
  int count = links_scan(text, strlen(text), spans, 8);
  munit_assert_int(count, ==, 3);
  for (int i = 0; i < count; i++) munit_assert_int(spans[i].kind, ==, LINK_PATH);
  munit_assert_int((int)spans[0].start, ==, 5);
  munit_assert_int((int)(spans[0].end - spans[0].start), ==,
                   (int)strlen("/usr/local/bin/tool"));
  return MUNIT_OK;
}

static MunitResult test_rejects_non_paths(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *text = "и/или 24/7 a/b and https://example.com/x";
  LinkSpan spans[8];
  int count = links_scan(text, strlen(text), spans, 8);
  munit_assert_int(count, ==, 1);
  munit_assert_int(spans[0].kind, ==, LINK_URL);
  return MUNIT_OK;
}

static MunitResult test_code_spans(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *text = "run `docs/notes.md` or `https://example.com/x`";
  LinkSpan spans[8];
  int count = links_scan(text, strlen(text), spans, 8);
  munit_assert_int(count, ==, 2);
  munit_assert_int(spans[0].kind, ==, LINK_PATH);
  munit_assert_int(spans[1].kind, ==, LINK_URL);
  /* Paths inside code are inert; web links stay actionable. */
  munit_assert_int(links_allowed(&spans[0], 1), ==, 0);
  munit_assert_int(links_allowed(&spans[0], 0), ==, 1);
  munit_assert_int(links_allowed(&spans[1], 1), ==, 1);
  return MUNIT_OK;
}

static MunitResult test_find_by_offset(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *text = "x https://a.dev y";
  LinkSpan spans[8];
  int count = links_scan(text, strlen(text), spans, 8);
  munit_assert_int(count, ==, 1);
  munit_assert_int(links_find(spans, count, 0), ==, -1);
  munit_assert_int(links_find(spans, count, spans[0].start), ==, 0);
  munit_assert_int(links_find(spans, count, spans[0].end - 1), ==, 0);
  munit_assert_int(links_find(spans, count, spans[0].end), ==, -1);
  return MUNIT_OK;
}

/* A link wrapped across rows keeps one span and stays hit-testable per row. */
static MunitResult test_wrapped_link(const MunitParameter params[], void *data) {
  (void)params;
  (void)data;
  const char *texts[] = {"go https://example.com/very/long/path/here now"};
  int roles[] = {0};
  linemap_build(NULL, roles, 1, texts, 20);
  LinkSpan spans[8];
  int count = links_scan(texts[0], strlen(texts[0]), spans, 8);
  munit_assert_int(count, ==, 1);
  int rows_with_link = 0;
  for (int i = 0; i < linemap_count(); i++) {
    const LineInfo *line = linemap_get(i);
    if (line->role < 0) continue;
    int hit = 0;
    for (int offset = line->byte_start; offset < line->byte_end; offset++)
      if (links_find(spans, count, (size_t)offset) >= 0) hit = 1;
    rows_with_link += hit;
  }
  munit_assert_int(rows_with_link, >, 1);
  linemap_free();
  return MUNIT_OK;
}

static MunitResult test_rejects_unknown_targets(const MunitParameter params[],
                                                void *data) {
  (void)params;
  (void)data;
  char error[64];
  /* A non-web scheme is refused before any command runs. */
  munit_assert_int(links_open("file:///etc/passwd", 17, LINK_URL, NULL, error,
                              sizeof(error)), ==, 0);
  munit_assert_string_equal(error, "Not a web link");
  munit_assert_int(links_open("/definitely/not/here.txt", 23, LINK_PATH, NULL,
                              error, sizeof(error)), ==, 0);
  munit_assert_string_equal(error, "Path not found");
  return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/scans_urls", test_scans_urls, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/scans_paths", test_scans_paths, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_non_paths", test_rejects_non_paths, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/code_spans", test_code_spans, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/find_by_offset", test_find_by_offset, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/wrapped_link", test_wrapped_link, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/rejects_unknown_targets", test_rejects_unknown_targets, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

MunitSuite links_suite = {"/links", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
