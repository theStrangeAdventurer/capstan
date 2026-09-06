#include "munit.h"
#include "markdown.h"
#include "linemap.h"
#include "text_layout.h"
#include "visual.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static void utf8_locale(void) {
  if (!setlocale(LC_CTYPE, "C.UTF-8"))
    munit_assert_not_null(setlocale(LC_CTYPE, "en_US.UTF-8"));
}

static void style_is(const MarkdownView *v, const char *word, int style) {
  const char *found = strstr(v->text, word);
  munit_assert_not_null(found);
  for (size_t i = 0; i < strlen(word); i++)
    munit_assert_int(v->styles[found - v->text + i], ==, style);
}

static MunitResult emphasis(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  MarkdownView v = {0};
  munit_assert_true(markdown_build(&v, "**жирный** и *курсив*, __bold__, _italic_, ***both***; **outer *inner* end**.", 80, NULL, NULL));
  munit_assert_string_equal(v.text, "жирный и курсив, bold, italic, both; outer inner end.");
  style_is(&v, "жирный", MARKDOWN_BOLD);
  style_is(&v, "курсив", MARKDOWN_ITALIC);
  style_is(&v, "bold", MARKDOWN_BOLD);
  style_is(&v, "italic", MARKDOWN_ITALIC);
  style_is(&v, "both", MARKDOWN_BOLD | MARKDOWN_ITALIC);
  style_is(&v, "inner", MARKDOWN_BOLD | MARKDOWN_ITALIC);
  style_is(&v, "end", MARKDOWN_BOLD);
  style_is(&v, ".", 0);
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult literal_and_streaming(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  MarkdownView v = {0};
  const char *source = "a_b_c snake_case \\*escaped\\* `**code**` and **unfinished";
  munit_assert_true(markdown_build(&v, source, 80, NULL, NULL));
  munit_assert_string_equal(v.text, "a_b_c snake_case *escaped* `**code**` and **unfinished");
  style_is(&v, "a_b_c", 0);
  style_is(&v, "*escaped*", 0);
  style_is(&v, "**code**", MARKDOWN_LITERAL);
  style_is(&v, "**unfinished", 0);
  munit_assert_true(markdown_build(&v, "**finished** plain", 80, NULL, NULL));
  munit_assert_string_equal(v.text, "finished plain");
  style_is(&v, "finished", MARKDOWN_BOLD);
  style_is(&v, "plain", 0);
  const char *fences = "````diff\n- **old**\n+ *new*\n```\n````\n~~~c\n| a | b |\n|---|---|\n~~~\n**after**";
  munit_assert_true(markdown_build(&v, fences, 80, NULL, NULL));
  munit_assert_not_null(strstr(v.text, "- **old**\n+ *new*\n```\n````"));
  munit_assert_not_null(strstr(v.text, "| a | b |\n|---|---|"));
  style_is(&v, "**old**", MARKDOWN_LITERAL);
  style_is(&v, "after", MARKDOWN_BOLD);
  munit_assert_true(markdown_build(&v, "```c\n**still streaming", 80, NULL, NULL));
  munit_assert_string_equal(v.text, "```c\n**still streaming");
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult fence_opening_rules(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  MarkdownView v = {0};
  const char *inline_sources[] = {
    "```code```\n**after**",
    "   ```code```\n**after**",
    "````code````\n**after**"
  };
  for (size_t i = 0; i < sizeof(inline_sources) / sizeof(inline_sources[0]); i++) {
    munit_assert_true(markdown_build(&v, inline_sources[i], 80, NULL, NULL));
    munit_assert_string_equal(v.text, "`code`\nafter");
    style_is(&v, "`code`", MARKDOWN_LITERAL);
    style_is(&v, "after", MARKDOWN_BOLD);
  }
  munit_assert_true(markdown_build(&v, "```lang`invalid\n**after**", 80, NULL, NULL));
  munit_assert_string_equal(v.text, "```lang`invalid\nafter");
  style_is(&v, "after", MARKDOWN_BOLD);

  const char *blocks[] = {
    "~~~lang`valid\n**inside**\n~~~\n**after**",
    "   ````c title\n**inside**\n   ````\n**after**"
  };
  for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
    munit_assert_true(markdown_build(&v, blocks[i], 80, NULL, NULL));
    size_t prefix = (size_t)(strstr(blocks[i], "**after**") - blocks[i]);
    munit_assert_memory_equal(prefix, v.text, blocks[i]);
    munit_assert_string_equal(v.text + prefix, "after");
    style_is(&v, "**inside**", MARKDOWN_LITERAL);
    style_is(&v, "after", MARKDOWN_BOLD);
  }
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult list_numbering(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const struct { const char *source, *expected; } cases[] = {
    {"0. **first**\n1. second", "0. first\n1. second"},
    {"0) first\n1) second", "0. first\n1. second"},
    {"5. first\n9. second", "5. first\n6. second"},
    {"- first\n- second", "- first\n- second"},
    {"0. first\n   - child\n   - next\n1. second",
     "0. first\n  - child\n  - next\n1. second"},
    /* A zero-started nested list needs a blank line: only 1 may interrupt
     * a paragraph under CommonMark. */
    {"- first\n\n  0. child\n  1. next\n- second",
     "- first\n  0. child\n  1. next\n- second"},
    {"0. first\n\n   0. child\n   1. next\n1. second",
     "0. first\n  0. child\n  1. next\n1. second"}
  };
  MarkdownView v = {0};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    munit_assert_true(markdown_build(&v, cases[i].source, 80, NULL, NULL));
    munit_assert_string_equal(v.text, cases[i].expected);
    if (i == 0) {
      style_is(&v, "first", MARKDOWN_BOLD);
      style_is(&v, "0. ", 0);
      munit_assert_size(v.source[0], ==, MARKDOWN_NO_SOURCE);
    }
  }
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult table(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  utf8_locale();
  MarkdownView v = {0};
  const char *source = "| Name | N |\n| :--- | ---: |\n| **Ada** | *7* |\n| Bob | 12 |";
  munit_assert_true(markdown_build(&v, source, 80, NULL, NULL));
  munit_assert_string_equal(v.text,
    "┌──────┬────┐\n"
    "│ Name │  N │\n"
    "├──────┼────┤\n"
    "│ Ada  │  7 │\n"
    "├──────┼────┤\n"
    "│ Bob  │ 12 │\n"
    "└──────┴────┘");
  style_is(&v, "Name", MARKDOWN_BOLD);
  style_is(&v, "Ada", MARKDOWN_BOLD);
  style_is(&v, "7", MARKDOWN_ITALIC);
  munit_assert_int(v.source[strstr(v.text, "Ada") - v.text], ==, strstr(source, "Ada") - source);
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult table_widths(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  utf8_locale();
  const char *source = "Имя | Значение\n:---: | ---\n界🙂 | **длинное значение abcdefghijklmnop**\né | хвост\n";
  MarkdownView v = {0};
  munit_assert_true(markdown_build(&v, source, 24, NULL, NULL));
  int expected = text_columns(v.text, strcspn(v.text, "\n"));
  munit_assert_int(expected, <=, 24);
  for (const char *p = v.text; *p;) {
    size_t n = strcspn(p, "\n");
    munit_assert_int(text_columns(p, n), ==, expected);
    p += n + (p[n] == '\n');
  }
  munit_assert_not_null(strstr(v.text, "界🙂"));
  munit_assert_not_null(strstr(v.text, "хвост"));
  munit_assert_true(markdown_build(&v, source, 8, NULL, NULL));
  munit_assert_null(strstr(v.text, "┌"));
  munit_assert_not_null(strstr(v.text, "Имя: 界🙂"));
  munit_assert_not_null(strstr(v.text, "Значение: длинное значение abcdefghijklmnop"));
  const char *texts[] = {v.text};
  int roles[] = {1};
  linemap_build(NULL, roles, 1, texts, 8);
  for (int i = 0; i < linemap_count(); i++) {
    const LineInfo *li = linemap_get(i);
    munit_assert_int(text_columns(v.text + li->byte_start, (size_t)(li->byte_end - li->byte_start)), <=, 8);
  }
  linemap_free();
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult table_edge_cases(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  MarkdownView v = {0};
  munit_assert_true(markdown_build(&v, "a | b\n-- | --\n", 80, NULL, NULL));
  munit_assert_not_null(strstr(v.text, "┌")); /* GFM accepts short delimiter cells. */
  munit_assert_true(markdown_build(&v, "a | b\nnot a delimiter\n", 80, NULL, NULL));
  munit_assert_string_equal(v.text, "a | b\nnot a delimiter\n");
  munit_assert_true(markdown_build(&v, "| A | B |\n|---|---|\n| a\\|b | |\n|x|y|extra|", 80, NULL, NULL));
  munit_assert_not_null(strstr(v.text, "a|b"));
  munit_assert_null(strstr(v.text, "extra")); /* GFM ignores excess cells. */
  munit_assert_true(markdown_build(&v, "| A | B |\n|---|---|\n|only|", 80, NULL, NULL));
  munit_assert_not_null(strstr(v.text, "only"));
  munit_assert_true(markdown_build(&v, "| A | B |\n|---|---|", 1, NULL, NULL));
  munit_assert_string_equal(v.text, "A\nB");
  markdown_free(&v);
  return MUNIT_OK;
}

typedef struct { const char *text; size_t start, end; } LiteralRange;
static int literal(size_t start, size_t end, void *data) {
  LiteralRange *range = data;
  return start < range->end && end >= range->start;
}

static MunitResult projection(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const char *source = "**Heading**\n[+] **raw** [exit 1]\n*after*";
  LiteralRange range = {source, (size_t)(strstr(source, "[+]") - source), (size_t)(strstr(source, "\n*after") - source)};
  MarkdownView v = {0};
  munit_assert_true(markdown_build(&v, source, 40, literal, &range));
  munit_assert_string_equal(v.text, "Heading\n[+] **raw** [exit 1]\nafter");
  style_is(&v, "**raw**", MARKDOWN_LITERAL);
  style_is(&v, "after", MARKDOWN_ITALIC);
  size_t marker = (size_t)(strstr(v.text, "[+]") - v.text);
  for (size_t i = 0; i < 3; i++) munit_assert_size(v.source[marker + i], ==, range.start + i);
  munit_assert_true(markdown_build(&v, "\n\n", 0, NULL, NULL));
  munit_assert_string_equal(v.text, "\n\n");
  munit_assert_true(markdown_build(&v, "", 0, NULL, NULL));
  munit_assert_string_equal(v.text, "");
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult literal_fence_closure(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const char *sources[] = {
    "```\n--- a/file\n+++ b/file\n@@ -1 +1 @@\n- **old**\n+ *new*\n```\n**after**",
    "~~~~\n--- a/file\n~~~\n````\n~~~~ trailing\n**inside**\n  ~~~~~ \t\n**after**",
    "````\n--- a/file\n```\n~~~~\n```` trailing\n**inside**\n  ````` \t\n**after**"
  };
  MarkdownView v = {0};
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
    const char *source = sources[i];
    size_t after = (size_t)(strstr(source, "**after**") - source);
    LiteralRange range = {source, (size_t)(strstr(source, "---") - source), after};
    munit_assert_true(markdown_build(&v, source, 80, literal, &range));
    munit_assert_size(v.length, ==, after + strlen("after"));
    munit_assert_memory_equal(after, v.text, source);
    for (size_t j = 0; j < after; j++) {
      munit_assert_int(v.styles[j], ==, MARKDOWN_LITERAL);
      munit_assert_size(v.source[j], ==, j);
    }
    munit_assert_string_equal(v.text + after, "after");
    style_is(&v, "after", MARKDOWN_BOLD);
  }
  /* A fence printed by a tool must not open a block in following prose. */
  const char *source = "```text\n**raw**\n**after**";
  LiteralRange range = {source, 0, (size_t)(strstr(source, "**after**") - source)};
  munit_assert_true(markdown_build(&v, source, 80, literal, &range));
  munit_assert_string_equal(v.text, "```text\n**raw**\nafter");
  style_is(&v, "**raw**", MARKDOWN_LITERAL);
  style_is(&v, "after", MARKDOWN_BOLD);
  markdown_free(&v);
  return MUNIT_OK;
}

static MunitResult unicode_layout(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  utf8_locale();
  const char *text = "a界é🙂b";
  munit_assert_int(text_columns(text, strlen(text)), ==, 7);
  munit_assert_size(text_line_length(text, 3), ==, strlen("a界"));
  munit_assert_size(text_line_length("éx", 1), ==, strlen("é"));
  munit_assert_size(text_line_length("界", 0), ==, strlen("界"));
  int roles[] = {1};
  const char **texts = malloc(sizeof(*texts));
  munit_assert_not_null(texts);
  texts[0] = text;
  linemap_build(NULL, roles, 1, texts, 3);
  visual_set_texts(texts, 1);
  munit_assert_int(linemap_get(1)->byte_end, ==, (int)strlen("a界"));
  munit_assert_int(linemap_get(2)->char_count, ==, 3); /* e, combining accent, emoji */
  munit_assert_int(visual_column_at_cell(1, 1), ==, 1);
  munit_assert_int(visual_column_at_cell(1, 2), ==, 1);
  munit_assert_int(visual_column_at_cell(1, 3), ==, 2);
  munit_assert_int(text_char_to_cell(text, strlen(text), 2), ==, 3);
  visual_reset();
  linemap_free();
  return MUNIT_OK;
}

static MunitResult wrapping_boundaries(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  utf8_locale();
  munit_assert_size(text_line_length("", 80), ==, 0);
  munit_assert_size(text_line_length("\nrest", 80), ==, 0);
  munit_assert_size(text_line_length("ab\nrest", 80), ==, 2);
  munit_assert_size(text_line_length("ab", 1), ==, 1);
  munit_assert_size(text_line_length("\xf0\x9f", 80), ==, 2);
  munit_assert_size(text_line_length("\xe7\nrest", 80), ==, 1);
  munit_assert_size(text_line_length("é́x", 1), ==, strlen("é́"));

  /* Repeated wraps of a newline-free message must not rescan its suffix. */
  size_t length = 4 * 1024 * 1024;
  char *text = malloc(length + 1);
  munit_assert_not_null(text);
  memset(text, 'x', length);
  text[length] = '\0';
  for (size_t offset = 0; offset < length;) {
    size_t expected = length - offset < 80 ? length - offset : 80;
    size_t n = text_line_length(text + offset, 80);
    munit_assert_size(n, ==, expected);
    offset += n;
  }
  free(text);
  return MUNIT_OK;
}

#define TEST(name) {"/" #name, name, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
static MunitTest tests[] = {
  TEST(emphasis), TEST(literal_and_streaming), TEST(fence_opening_rules),
  TEST(list_numbering), TEST(table), TEST(table_widths),
  TEST(table_edge_cases), TEST(projection), TEST(literal_fence_closure),
  TEST(unicode_layout), TEST(wrapping_boundaries),
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite markdown_suite = {"/markdown", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
