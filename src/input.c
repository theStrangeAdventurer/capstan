#include "input.h"
#include "utils.h"
#include "text_layout.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_input_buf[INPUT_BUFFER_SIZE] = {0};
static char g_display_buf[INPUT_DISPLAY_BUFFER_SIZE] = {0};
static int g_cursor = 0;
static int g_paste_active = 0;
static size_t g_paste_matched = 0;
static InputImage *g_images = NULL;
static size_t g_image_count = 0;
static size_t g_image_capacity = 0;

#define INPUT_MAX_IMAGES 16

typedef struct {
  int start, end, characters;
} PasteSpan;
static PasteSpan *g_pastes;
static size_t g_paste_count, g_paste_capacity;
static int g_paste_start, g_paste_width = 80;

static void reset_pastes(void) {
  free(g_pastes);
  g_pastes = NULL;
  g_paste_count = g_paste_capacity = 0;
  g_paste_active = 0;
  g_paste_matched = 0;
}

static int paste_label(char *out, const PasteSpan *span) {
  return snprintf(out, 24, "[Pasted %d chars]", span->characters);
}

static void finish_paste(void) {
  int end = g_cursor;
  if (end <= g_paste_start) return;
  /* Bound layout to the pasted substring, not the draft suffix. */
  char saved = g_input_buf[end];
  g_input_buf[end] = '\0';
  const char *p = g_input_buf + g_paste_start;
  int rows = 1, characters = 0;
  for (size_t i = 0, length = strlen(p); i < length;) {
    int columns;
    i += text_character(p + i, length - i, &columns);
    characters++;
  }
  while (*p) {
    p += text_line_length(p, g_paste_width);
    if (*p == '\n') { p++; rows++; }
    else if (*p) rows++;
  }
  g_input_buf[end] = saved;
  if (rows <= 2) return;
  if (g_paste_count == g_paste_capacity) {
    size_t next = g_paste_capacity ? g_paste_capacity * 2 : 4;
    PasteSpan *grown = realloc(g_pastes, next * sizeof(*grown));
    if (!grown) return; /* Keep the complete raw paste expanded. */
    g_pastes = grown;
    g_paste_capacity = next;
  }
  size_t at = 0;
  while (at < g_paste_count && g_pastes[at].start < g_paste_start) at++;
  memmove(g_pastes + at + 1, g_pastes + at,
          (g_paste_count - at) * sizeof(*g_pastes));
  g_pastes[at] = (PasteSpan){g_paste_start, end, characters};
  g_paste_count++;
}

static void input_delete_range(int start, int end);

void input_images_free(InputImage *images, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(images[i].mime_type);
    free(images[i].data);
  }
  free(images);
}

void input_init(void) {
  reset_pastes();
  g_paste_width = 80;
  memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
  memset(g_display_buf, 0, sizeof(g_display_buf));
  g_cursor = 0;
  g_paste_active = 0;
  g_paste_matched = 0;
  input_images_free(g_images, g_image_count);
  g_images = NULL;
  g_image_count = 0;
  g_image_capacity = 0;
}

const char *input_get_text(void) { return g_input_buf; }

static int image_prefix(char *out, size_t out_size) {
  size_t used = 0;
  for (size_t i = 0; i < g_image_count; i++) {
    int n = snprintf(out + used, out_size - used, "[Image %zu] ", i + 1);
    if (n < 0 || (size_t)n >= out_size - used)
      break;
    used += (size_t)n;
  }
  return (int)used;
}

const char *input_get_display_text(void) {
  int prefix = image_prefix(g_display_buf, sizeof(g_display_buf));
  int raw = 0, used = prefix;
  for (size_t i = 0; i < g_paste_count; i++) {
    PasteSpan *span = &g_pastes[i];
    int length = span->start - raw;
    memcpy(g_display_buf + used, g_input_buf + raw, (size_t)length);
    used += length;
    used += paste_label(g_display_buf + used, span);
    raw = span->end;
  }
  strcpy(g_display_buf + used, g_input_buf + raw);
  return g_display_buf;
}

const char *input_get_message_text(void) {
  int prefix = image_prefix(g_display_buf, sizeof(g_display_buf));
  snprintf(g_display_buf + prefix, sizeof(g_display_buf) - (size_t)prefix,
           "%s", g_input_buf);
  return g_display_buf;
}

int input_get_cursor(void) { return g_cursor; }

int input_get_display_cursor(void) {
  char prefix[INPUT_BUFFER_SIZE];
  int cursor = image_prefix(prefix, sizeof(prefix)) + g_cursor;
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].end > g_cursor) break;
    char label[24];
    cursor += paste_label(label, &g_pastes[i]) -
              (g_pastes[i].end - g_pastes[i].start);
  }
  return cursor;
}

void input_insert(int ch) {
  if (ch < 0 || ch > 0xFF || g_cursor < 0 ||
      g_cursor >= INPUT_BUFFER_SIZE - 1)
    return;
  size_t tail_len = strlen(g_input_buf + g_cursor) + 1;
  if ((size_t)g_cursor + tail_len >= INPUT_BUFFER_SIZE)
    return;
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].start >= g_cursor) {
      g_pastes[i].start++;
      g_pastes[i].end++;
    }
  }
  memmove(g_input_buf + g_cursor + 1, g_input_buf + g_cursor, tail_len);
  g_input_buf[g_cursor++] = (char)ch;
}

void input_set_paste_width(int width) {
  g_paste_width = width > 0 ? width : 1;
}

void input_paste_begin(void) {
  if (g_paste_active) return;
  g_paste_start = g_cursor;
  g_paste_active = 1;
  g_paste_matched = 0;
}

int input_paste_active(void) { return g_paste_active; }

int input_paste_feed(int ch) {
  static const char end_marker[] = "\033[201~";
  if (!g_paste_active || ch < 0 || ch > 0xFF)
    return 0;

  if (ch == (unsigned char)end_marker[g_paste_matched]) {
    if (++g_paste_matched == sizeof(end_marker) - 1) {
      finish_paste();
      g_paste_active = 0;
      g_paste_matched = 0;
    }
    return 1;
  }

  /* A partial marker was literal text. Reconsider this byte so a second
     ESC can still begin the real end marker. Never interpret pasted keys
     as shortcuts or submit pasted newlines. */
  for (size_t i = 0; i < g_paste_matched; i++)
    input_insert((unsigned char)end_marker[i]);
  g_paste_matched = 0;
  if (ch == (unsigned char)end_marker[0])
    g_paste_matched = 1;
  else if (ch != 0)
    input_insert(ch == '\r' ? '\n' : ch);
  return 1;
}

void input_set_text(const char *text) {
  reset_pastes();
  strncpy(g_input_buf, text, INPUT_BUFFER_SIZE - 1);
  g_input_buf[INPUT_BUFFER_SIZE - 1] = '\0';
  g_cursor = (int)strlen(g_input_buf);
}

void input_backspace(void) {
  if (g_cursor > 0) {
    int prev_pos = get_prev_char_start(g_input_buf, g_cursor);
    input_delete_range(prev_pos, g_cursor);
  } else if (g_image_count > 0) {
    free(g_images[g_image_count - 1].mime_type);
    free(g_images[g_image_count - 1].data);
    g_image_count--;
  }
}

static unsigned int input_codepoint_at(int pos) {
  const unsigned char *s = (const unsigned char *)g_input_buf + pos;
  if (s[0] < 0x80)
    return s[0];
  if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
    return ((unsigned int)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
  if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
      (s[2] & 0xC0) == 0x80)
    return ((unsigned int)(s[0] & 0x0F) << 12) |
           ((unsigned int)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
      (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
    return ((unsigned int)(s[0] & 0x07) << 18) |
           ((unsigned int)(s[1] & 0x3F) << 12) |
           ((unsigned int)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  return s[0];
}

static int input_unicode_is_space(unsigned int cp) {
  return cp == 0x00A0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static int input_unicode_is_punctuation(unsigned int cp) {
  return cp == 0x00AB || cp == 0x00BB ||
         (cp >= 0x2010 && cp <= 0x206F) ||
         (cp >= 0x2E00 && cp <= 0x2E7F) ||
         (cp >= 0x3001 && cp <= 0x303F) ||
         (cp >= 0xFE10 && cp <= 0xFE1F) ||
         (cp >= 0xFE30 && cp <= 0xFE4F) ||
         (cp >= 0xFF01 && cp <= 0xFF0F) ||
         (cp >= 0xFF1A && cp <= 0xFF20) ||
         (cp >= 0xFF3B && cp <= 0xFF40) ||
         (cp >= 0xFF5B && cp <= 0xFF65);
}

static int input_char_class(int pos) {
  unsigned char ch = (unsigned char)g_input_buf[pos];
  if (ch < 0x80) {
    if (isalnum(ch) || ch == '_')
      return 1;
    return isspace(ch) ? 0 : 2;
  }
  unsigned int cp = input_codepoint_at(pos);
  if (input_unicode_is_space(cp))
    return 0;
  return input_unicode_is_punctuation(cp) ? 2 : 1;
}

static void input_delete_range(int start, int end) {
  if (start < 0 || end <= start || end > g_cursor)
    return;
  /* Any touched block is removed whole, including word/line deletions. */
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].start < end && g_pastes[i].end > start) {
      if (g_pastes[i].start < start) start = g_pastes[i].start;
      if (g_pastes[i].end > end) end = g_pastes[i].end;
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < g_paste_count; i++) {
    PasteSpan span = g_pastes[i];
    if (span.start < end && span.end > start) continue;
    if (span.start >= end) {
      span.start -= end - start;
      span.end -= end - start;
    }
    g_pastes[kept++] = span;
  }
  g_paste_count = kept;
  size_t tail_len = strlen(g_input_buf + end) + 1;
  memmove(g_input_buf + start, g_input_buf + end, tail_len);
  g_cursor = start;
}

void input_delete_word_backward(void) {
  if (g_cursor <= 0)
    return;

  int start = g_cursor;
  while (start > 0) {
    int prev = get_prev_char_start(g_input_buf, start);
    if (input_char_class(prev) != 0)
      break;
    start = prev;
  }
  if (start > 0) {
    int prev = get_prev_char_start(g_input_buf, start);
    int char_class = input_char_class(prev);
    do {
      start = prev;
      if (start <= 0)
        break;
      prev = get_prev_char_start(g_input_buf, start);
    } while (input_char_class(prev) == char_class);
  }
  input_delete_range(start, g_cursor);
}

void input_delete_to_line_start(void) {
  int start = g_cursor;
  /* A hidden trailing newline is part of the visible paste block. */
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].end == start) {
      start--;
      break;
    }
  }
  while (start > 0 && g_input_buf[start - 1] != '\n')
    start--;
  input_delete_range(start, g_cursor);
}

void input_move_left(void) {
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].end == g_cursor) {
      g_cursor = g_pastes[i].start;
      return;
    }
  }
  if (g_cursor > 0)
    g_cursor = get_prev_char_start(g_input_buf, g_cursor);
}

void input_move_right(void) {
  for (size_t i = 0; i < g_paste_count; i++) {
    if (g_pastes[i].start == g_cursor) {
      g_cursor = g_pastes[i].end;
      return;
    }
  }
  if (g_input_buf[g_cursor]) {
    g_cursor++;
    while (g_input_buf[g_cursor] && (g_input_buf[g_cursor] & 0xC0) == 0x80)
      g_cursor++;
  }
}

void input_clear(void) {
  reset_pastes();
  memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
  g_cursor = 0;
  input_images_free(g_images, g_image_count);
  g_images = NULL;
  g_image_count = 0;
  g_image_capacity = 0;
}

int input_add_image(const char *mime_type, const char *base64_data) {
  if (!mime_type || !base64_data || !mime_type[0] || !base64_data[0] ||
      g_image_count >= INPUT_MAX_IMAGES)
    return 0;
  if (g_image_count == g_image_capacity) {
    size_t next = g_image_capacity ? g_image_capacity * 2 : 2;
    InputImage *grown = realloc(g_images, next * sizeof(*grown));
    if (!grown)
      return 0;
    g_images = grown;
    g_image_capacity = next;
  }
  InputImage image = {my_strdup(mime_type), my_strdup(base64_data)};
  if (!image.mime_type || !image.data) {
    free(image.mime_type);
    free(image.data);
    return 0;
  }
  g_images[g_image_count++] = image;
  return 1;
}

size_t input_image_count(void) { return g_image_count; }

InputImage *input_take_images(size_t *count) {
  InputImage *images = g_images;
  if (count)
    *count = g_image_count;
  g_images = NULL;
  g_image_count = 0;
  g_image_capacity = 0;
  return images;
}
