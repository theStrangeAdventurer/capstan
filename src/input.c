#include "input.h"
#include "utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_input_buf[INPUT_BUFFER_SIZE] = {0};
static char g_display_buf[INPUT_DISPLAY_BUFFER_SIZE] = {0};
static int g_cursor = 0;
static InputImage *g_images = NULL;
static size_t g_image_count = 0;
static size_t g_image_capacity = 0;

#define INPUT_MAX_IMAGES 16

void input_images_free(InputImage *images, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(images[i].mime_type);
    free(images[i].data);
  }
  free(images);
}

void input_init(void) {
  memset(g_input_buf, 0, INPUT_BUFFER_SIZE);
  memset(g_display_buf, 0, sizeof(g_display_buf));
  g_cursor = 0;
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
  snprintf(g_display_buf + prefix, sizeof(g_display_buf) - (size_t)prefix,
           "%s", g_input_buf);
  return g_display_buf;
}

int input_get_cursor(void) { return g_cursor; }

int input_get_display_cursor(void) {
  char prefix[INPUT_BUFFER_SIZE];
  return image_prefix(prefix, sizeof(prefix)) + g_cursor;
}

void input_insert(int ch) {
  if (ch < 0 || ch > 0xFF || g_cursor < 0 ||
      g_cursor >= INPUT_BUFFER_SIZE - 1)
    return;
  size_t tail_len = strlen(g_input_buf + g_cursor) + 1;
  if ((size_t)g_cursor + tail_len >= INPUT_BUFFER_SIZE)
    return;
  memmove(g_input_buf + g_cursor + 1, g_input_buf + g_cursor, tail_len);
  g_input_buf[g_cursor++] = (char)ch;
}

void input_set_text(const char *text) {
  strncpy(g_input_buf, text, INPUT_BUFFER_SIZE - 1);
  g_input_buf[INPUT_BUFFER_SIZE - 1] = '\0';
  g_cursor = (int)strlen(g_input_buf);
}

void input_backspace(void) {
  if (g_cursor > 0) {
    int prev_pos = get_prev_char_start(g_input_buf, g_cursor);
    int tail_len = strlen(g_input_buf + g_cursor) + 1;
    memmove(g_input_buf + prev_pos, g_input_buf + g_cursor, tail_len);
    g_cursor = prev_pos;
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
  while (start > 0 && g_input_buf[start - 1] != '\n')
    start--;
  input_delete_range(start, g_cursor);
}

void input_move_left(void) {
  if (g_cursor > 0)
    g_cursor = get_prev_char_start(g_input_buf, g_cursor);
}

void input_move_right(void) {
  if (g_input_buf[g_cursor]) {
    g_cursor++;
    while (g_input_buf[g_cursor] && (g_input_buf[g_cursor] & 0xC0) == 0x80)
      g_cursor++;
  }
}

void input_clear(void) {
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
