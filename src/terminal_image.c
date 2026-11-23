#include "terminal_image.h"
#include "clipboard.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_FILE_LIMIT (16u * 1024u * 1024u)
#define IMAGE_CACHE_SLOTS 4
#define KITTY_CHUNK 4096

typedef struct {
  char *path;
  unsigned char *data;
  size_t length;
} ImageFile;

static ImageFile g_files[IMAGE_CACHE_SLOTS];
static int g_next_slot;

typedef struct {
  char *path;
  int row, col, cols, rows;
} Placement;

static Placement *g_drawn;
static int g_drawn_count;

static int contains_ci(const char *value, const char *needle) {
  for (; *value; value++) {
    const char *a = value, *b = needle;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      a++;
      b++;
    }
    if (!*b) return 1;
  }
  return 0;
}

static int env_contains(const char *name, const char *needle) {
  const char *value = getenv(name);
  return value && *value && contains_ci(value, needle);
}

static int version_at_least(const char *value, int major, int minor) {
  if (!value || !*value) return 0;
  int seen_major = atoi(value);
  const char *dot = strchr(value, '.');
  int seen_minor = dot ? atoi(dot + 1) : 0;
  return seen_major > major || (seen_major == major && seen_minor >= minor);
}

int terminal_image_enabled(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  const char *force = getenv("CAPSTAN_TERMINAL_IMAGES");
  if (force && *force) {
    if (!strcmp(force, "off") || !strcmp(force, "0") || !strcmp(force, "none"))
      return cached = 0;
    if (!strcmp(force, "on") || !strcmp(force, "1") || !strcmp(force, "kitty"))
      return cached = 1;
  }
  /* Multiplexers that do not keep graphics pane-local without their own support. */
  if (getenv("TMUX") || getenv("ZELLIJ")) return cached = 0;
  if (getenv("KITTY_WINDOW_ID") || getenv("WEZTERM_EXECUTABLE") ||
      getenv("GHOSTTY_RESOURCES_DIR"))
    return cached = 1;
  if (env_contains("TERM", "kitty") || env_contains("TERM", "ghostty") ||
      env_contains("TERM", "wezterm"))
    return cached = 1;
  if (env_contains("TERM_PROGRAM", "kitty") ||
      env_contains("TERM_PROGRAM", "ghostty") ||
      env_contains("TERM_PROGRAM", "wezterm"))
    return cached = 1;
  if (env_contains("TERM_PROGRAM", "iterm"))
    return cached = version_at_least(getenv("TERM_PROGRAM_VERSION"), 3, 6);
  return cached = 0;
}

void terminal_image_cell_size(int *width, int *height) {
  int w = 8, h = 16;
#ifdef TIOCGWINSZ
  struct winsize size;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
      size.ws_row > 0 && size.ws_xpixel > 0 && size.ws_ypixel > 0) {
    int cw = (int)size.ws_xpixel / (int)size.ws_col;
    int ch = (int)size.ws_ypixel / (int)size.ws_row;
    if (cw > 0 && ch > 0) {
      w = cw;
      h = ch;
    }
  }
#endif
  *width = w;
  *height = h;
}

static void file_cache_free(void) {
  for (int i = 0; i < IMAGE_CACHE_SLOTS; i++) {
    free(g_files[i].path);
    free(g_files[i].data);
  }
  memset(g_files, 0, sizeof(g_files));
  g_next_slot = 0;
}

static void placement_cache_free(void) {
  for (int i = 0; i < g_drawn_count; i++) free(g_drawn[i].path);
  free(g_drawn);
  g_drawn = NULL;
  g_drawn_count = 0;
}

void terminal_image_reset(void) {
  file_cache_free();
  placement_cache_free();
}

static int read_small_file(const char *path, unsigned char **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;
  struct stat info;
  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return 0;
  if (info.st_size <= 0 || (unsigned long long)info.st_size > IMAGE_FILE_LIMIT) return 0;
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  size_t size = (size_t)info.st_size;
  unsigned char *data = malloc(size);
  if (!data) {
    fclose(file);
    return 0;
  }
  size_t got = fread(data, 1, size, file);
  fclose(file);
  if (got != size) {
    free(data);
    return 0;
  }
  *out = data;
  *out_len = size;
  return 1;
}

/* LRU-less ring: the visible set is tiny, so the newest four entries suffice. */
static int image_file(const char *path, const unsigned char **data, size_t *length) {
  for (int i = 0; i < IMAGE_CACHE_SLOTS; i++) {
    if (g_files[i].path && !strcmp(g_files[i].path, path)) {
      *data = g_files[i].data;
      *length = g_files[i].length;
      return 1;
    }
  }
  unsigned char *bytes = NULL;
  size_t size = 0;
  if (!read_small_file(path, &bytes, &size)) return 0;
  ImageFile *slot = &g_files[g_next_slot];
  g_next_slot = (g_next_slot + 1) % IMAGE_CACHE_SLOTS;
  free(slot->path);
  free(slot->data);
  slot->path = malloc(strlen(path) + 1);
  if (!slot->path) {
    free(bytes);
    slot->data = NULL;
    slot->length = 0;
    return 0;
  }
  memcpy(slot->path, path, strlen(path) + 1);
  slot->data = bytes;
  slot->length = size;
  *data = slot->data;
  *length = slot->length;
  return 1;
}

static const char *image_extension(const char *path) {
  static const char *known[] = {"png", "jpg", "jpeg", "gif", "webp"};
  const char *dot = strrchr(path, '.');
  if (!dot || !dot[1]) return NULL;
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (!strcasecmp(dot + 1, known[i])) return known[i];
  }
  return NULL;
}

static int magic_matches(const char *extension, const unsigned char *data, size_t size) {
  if (!strcmp(extension, "png"))
    return size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8);
  if (!strcmp(extension, "jpg") || !strcmp(extension, "jpeg"))
    return size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
  if (!strcmp(extension, "gif"))
    return size >= 6 && !memcmp(data, "GIF8", 4);
  if (!strcmp(extension, "webp"))
    return size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4);
  return 0;
}

static int trimmed_char(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '"' || c == '\'' ||
         c == '`' || c == '(' || c == ')' || c == '[' || c == ']' || c == '<' ||
         c == '>' || c == '*' || c == ',';
}

int terminal_image_line(const char *line, size_t len, char *path, size_t cap) {
  if (!line || !len || !cap) return 0;
  size_t start = 0, end = len;
  while (start < end && (line[start] == ' ' || line[start] == '\t')) start++;
  /* Markdown projection prefixes: block quotes and list markers. */
  while (start < end && line[start] == '>') {
    start++;
    while (start < end && (line[start] == ' ' || line[start] == '\t')) start++;
  }
  if (start + 1 < end &&
      (line[start] == '-' || line[start] == '*' || line[start] == '+') &&
      (line[start + 1] == ' ' || line[start + 1] == '\t')) {
    start += 2;
  } else {
    size_t digits = start;
    while (digits < end && isdigit((unsigned char)line[digits])) digits++;
    if (digits > start && digits + 1 < end && line[digits] == '.' &&
        line[digits + 1] == ' ')
      start = digits + 2;
  }
  while (start < end && trimmed_char(line[start])) start++;
  while (end > start && trimmed_char(line[end - 1])) end--;
  while (end > start && (line[end - 1] == '.' || line[end - 1] == ':')) end--;
  size_t length = end - start;
  if (!length || length >= cap) return 0;
  for (size_t i = 0; i < length; i++) {
    if (line[start + i] == ' ' || line[start + i] == '\t') return 0;
  }
  memcpy(path, line + start, length);
  path[length] = '\0';
  const char *extension = image_extension(path);
  if (!extension) return 0;
  const unsigned char *data = NULL;
  size_t size = 0;
  if (!image_file(path, &data, &size)) return 0;
  return magic_matches(extension, data, size);
}

static unsigned be32(const unsigned char *p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) |
         (unsigned)p[3];
}

static int parse_png(const unsigned char *data, size_t size, int *w, int *h) {
  if (size < 24 || memcmp(data + 12, "IHDR", 4) != 0) return 0;
  *w = (int)be32(data + 16);
  *h = (int)be32(data + 20);
  return 1;
}

static int parse_jpeg(const unsigned char *data, size_t size, int *w, int *h) {
  size_t pos = 2;
  while (pos + 9 < size) {
    if (data[pos] != 0xff) {
      pos++;
      continue;
    }
    unsigned marker = data[pos + 1];
    if (marker == 0xff) {
      pos++;
      continue;
    }
    if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      pos += 2;
      continue;
    }
    if (marker == 0xd9 || marker == 0xda) return 0;
    unsigned length = ((unsigned)data[pos + 2] << 8) | data[pos + 3];
    if (length < 2) return 0;
    if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
        marker != 0xcc) {
      *h = ((int)data[pos + 5] << 8) | data[pos + 6];
      *w = ((int)data[pos + 7] << 8) | data[pos + 8];
      return *w > 0 && *h > 0;
    }
    pos += 2 + length;
  }
  return 0;
}

static int parse_gif(const unsigned char *data, size_t size, int *w, int *h) {
  if (size < 10) return 0;
  *w = data[6] | (data[7] << 8);
  *h = data[8] | (data[9] << 8);
  return *w > 0 && *h > 0;
}

static int parse_webp(const unsigned char *data, size_t size, int *w, int *h) {
  if (size < 30) return 0;
  if (!memcmp(data + 12, "VP8 ", 4)) {
    if (memcmp(data + 23, "\x9d\x01\x2a", 3) != 0) return 0;
    *w = (data[26] | (data[27] << 8)) & 0x3fff;
    *h = (data[28] | (data[29] << 8)) & 0x3fff;
    return *w > 0 && *h > 0;
  }
  if (!memcmp(data + 12, "VP8L", 4)) {
    if (data[20] != 0x2f) return 0;
    unsigned bits = (unsigned)data[21] | ((unsigned)data[22] << 8) |
                    ((unsigned)data[23] << 16) | ((unsigned)data[24] << 24);
    *w = (int)(bits & 0x3fff) + 1;
    *h = (int)((bits >> 14) & 0x3fff) + 1;
    return 1;
  }
  if (!memcmp(data + 12, "VP8X", 4)) {
    *w = 1 + (data[24] | (data[25] << 8) | (data[26] << 16));
    *h = 1 + (data[27] | (data[28] << 8) | (data[29] << 16));
    return 1;
  }
  return 0;
}

int terminal_image_mime(const char *path, char *out, size_t cap) {
  const char *extension = image_extension(path);
  if (!extension || !out || !cap) return 0;
  const unsigned char *data = NULL;
  size_t size = 0;
  if (!image_file(path, &data, &size)) return 0;
  if (!magic_matches(extension, data, size)) return 0;
  const char *mime = !strcmp(extension, "png")    ? "image/png"
                     : !strcmp(extension, "gif")  ? "image/gif"
                     : !strcmp(extension, "webp") ? "image/webp"
                                                  : "image/jpeg";
  if (strlen(mime) + 1 > cap) return 0;
  memcpy(out, mime, strlen(mime) + 1);
  return 1;
}

int terminal_image_size(const char *path, int *width, int *height) {
  if (!path || !width || !height) return 0;
  const char *extension = image_extension(path);
  if (!extension) return 0;
  const unsigned char *data = NULL;
  size_t size = 0;
  if (!image_file(path, &data, &size)) return 0;
  if (!magic_matches(extension, data, size)) return 0;
  int w = 0, h = 0;
  int ok = !strcmp(extension, "png")   ? parse_png(data, size, &w, &h)
           : !strcmp(extension, "gif") ? parse_gif(data, size, &w, &h)
           : !strcmp(extension, "webp") ? parse_webp(data, size, &w, &h)
                                        : parse_jpeg(data, size, &w, &h);
  if (!ok || w <= 0 || h <= 0) return 0;
  *width = w;
  *height = h;
  return 1;
}

int terminal_image_box(int avail_cols, int max_rows, int cell_w, int cell_h,
                       int img_w, int img_h, TerminalImageBox *out) {
  if (avail_cols < 1 || cell_w < 1 || cell_h < 1 || img_w < 1 || img_h < 1 ||
      !out)
    return 0;
  long budget = (long)avail_cols * cell_w;
  long px = budget < TERMINAL_IMAGE_MAX_PX ? budget : TERMINAL_IMAGE_MAX_PX;
  if (px < 1) return 0;
  long scaled_h = px * img_h / img_w;
  if (scaled_h < 1) scaled_h = 1;
  long cols = (px + cell_w - 1) / cell_w;
  long rows = (scaled_h + cell_h - 1) / cell_h;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols > avail_cols) cols = avail_cols;
  /* Tall images shrink instead of overflowing the pane: keep the aspect ratio. */
  if (max_rows > 0 && rows > max_rows) {
    cols = cols * max_rows / rows;
    if (cols < 1) cols = 1;
    rows = max_rows;
    px = cols * cell_w;
  }
  out->cols = (int)cols;
  out->rows = (int)rows;
  out->width_px = (int)px;
  return 1;
}

static int placements_match(const TerminalImage *items, int count) {
  if (count != g_drawn_count) return 0;
  for (int i = 0; i < count; i++) {
    const Placement *drawn = &g_drawn[i];
    if (drawn->row != items[i].row || drawn->col != items[i].col ||
        drawn->cols != items[i].box.cols || drawn->rows != items[i].box.rows ||
        strcmp(drawn->path, items[i].path) != 0)
      return 0;
  }
  return 1;
}

static int remember_placements(const TerminalImage *items, int count) {
  placement_cache_free();
  if (count <= 0) return 1;
  g_drawn = calloc((size_t)count, sizeof(*g_drawn));
  if (!g_drawn) return 0;
  for (int i = 0; i < count; i++) {
    size_t length = strlen(items[i].path);
    g_drawn[i].path = malloc(length + 1);
    if (!g_drawn[i].path) {
      g_drawn_count = i;
      placement_cache_free();
      return 0;
    }
    memcpy(g_drawn[i].path, items[i].path, length + 1);
    g_drawn[i].row = items[i].row;
    g_drawn[i].col = items[i].col;
    g_drawn[i].cols = items[i].box.cols;
    g_drawn[i].rows = items[i].box.rows;
  }
  g_drawn_count = count;
  return 1;
}

/* Kitty graphics: transmit-and-display the pixels in 4096-byte chunks, sized in
 * cells so the reserved rows match the placement exactly. */
static void emit_image(FILE *out, const TerminalImage *item) {
  const unsigned char *data = NULL;
  size_t size = 0;
  if (!image_file(item->path, &data, &size)) return;
  char *payload = clipboard_base64_encode(data, size);
  if (!payload) return;
  fprintf(out, "\0337\033[%d;%dH", item->row, item->col);
  size_t length = strlen(payload);
  size_t offset = 0;
  do {
    size_t chunk = length - offset < KITTY_CHUNK ? length - offset : KITTY_CHUNK;
    int more = offset + chunk < length;
    if (offset == 0)
      fprintf(out, "\033_Ga=T,f=100,c=%d,r=%d,q=2,m=%d;", item->box.cols,
              item->box.rows, more);
    else
      fprintf(out, "\033_Gm=%d;", more);
    fwrite(payload + offset, 1, chunk, out);
    fputs("\033\\", out);
    offset += chunk;
  } while (offset < length);
  fputs("\0338", out);
  free(payload);
}

void terminal_image_draw(FILE *out, const TerminalImage *items, int count) {
  if (!out) return;
  if (count < 0) count = 0;
  if (placements_match(items, count)) return;
  /* Clearing first keeps scrolled-away placements from lingering. */
  fputs("\033_Ga=d,d=A,q=2;\033\\", out);
  for (int i = 0; i < count; i++) emit_image(out, &items[i]);
  fflush(out);
  remember_placements(items, count);
}
