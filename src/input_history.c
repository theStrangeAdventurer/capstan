#include "input_history.h"
#include "app_config.h"
#include "input.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *g_entries[INPUT_HISTORY_LIMIT];
static int g_count = 0;
static int g_browse_index = -1;
static char *g_draft = NULL;
static char g_path[PATH_MAX] = "";

static char *dup_string(const char *s) {
  size_t len = strlen(s ? s : "");
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, s ? s : "", len + 1);
  return copy;
}

static unsigned long hash_workdir(const char *text) {
  unsigned long h = 1469598103934665603UL;
  text = text ? text : "";
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    h ^= (unsigned long)*p;
    h *= 1099511628211UL;
  }
  return h;
}

static void clear_entries(void) {
  for (int i = 0; i < g_count; i++) {
    free(g_entries[i]);
    g_entries[i] = NULL;
  }
  g_count = 0;
}

static void reset_browse(void) {
  g_browse_index = -1;
  free(g_draft);
  g_draft = NULL;
}

void input_history_reset(void) {
  clear_entries();
  reset_browse();
  g_path[0] = '\0';
}

static int ensure_history_dir(char *dir, size_t dir_size) {
  if (app_state_ensure_dir() != 0)
    return 0;
  char state[PATH_MAX];
  if (app_state_dir(state, sizeof(state)) != 0)
    return 0;
  int n = snprintf(dir, dir_size, "%s/history", state);
  if (n < 0 || (size_t)n >= dir_size)
    return 0;
  mkdir(dir, 0755);
  return 1;
}

static int set_history_path(const char *workdir) {
  char dir[PATH_MAX];
  if (!ensure_history_dir(dir, sizeof(dir)))
    return 0;
  unsigned long h = hash_workdir(workdir);
  int n = snprintf(g_path, sizeof(g_path), "%s/%016lx.jsonl", dir, h);
  if (n < 0 || (size_t)n >= sizeof(g_path)) {
    g_path[0] = '\0';
    return 0;
  }
  return 1;
}

static int append_char(char **out, size_t *len, size_t *cap, char ch) {
  if (*len + 1 >= *cap) {
    size_t next = *cap ? *cap * 2 : 64;
    char *grown = realloc(*out, next);
    if (!grown)
      return 0;
    *out = grown;
    *cap = next;
  }
  (*out)[(*len)++] = ch;
  (*out)[*len] = '\0';
  return 1;
}

static char *json_escape_string(const char *text) {
  char *out = NULL;
  size_t len = 0, cap = 0;
  if (!append_char(&out, &len, &cap, '"'))
    return NULL;
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;
       p++) {
    if (*p == '"' || *p == '\\') {
      if (!append_char(&out, &len, &cap, '\\') ||
          !append_char(&out, &len, &cap, (char)*p))
        goto fail;
    } else if (*p == '\n') {
      if (!append_char(&out, &len, &cap, '\\') ||
          !append_char(&out, &len, &cap, 'n'))
        goto fail;
    } else if (*p == '\r') {
      if (!append_char(&out, &len, &cap, '\\') ||
          !append_char(&out, &len, &cap, 'r'))
        goto fail;
    } else if (*p == '\t') {
      if (!append_char(&out, &len, &cap, '\\') ||
          !append_char(&out, &len, &cap, 't'))
        goto fail;
    } else if (*p < 32) {
      if (!append_char(&out, &len, &cap, '?'))
        goto fail;
    } else if (!append_char(&out, &len, &cap, (char)*p)) {
      goto fail;
    }
  }
  if (!append_char(&out, &len, &cap, '"'))
    goto fail;
  return out;
fail:
  free(out);
  return NULL;
}

static char *json_unescape_line(const char *line) {
  if (!line || line[0] != '"')
    return NULL;
  char *out = NULL;
  size_t len = 0, cap = 0;
  for (const char *p = line + 1; *p; p++) {
    if (*p == '"')
      return out ? out : dup_string("");
    if (*p == '\\') {
      p++;
      if (!*p)
        break;
      if (*p == 'n') {
        if (!append_char(&out, &len, &cap, '\n'))
          goto fail;
      } else if (*p == 'r') {
        if (!append_char(&out, &len, &cap, '\r'))
          goto fail;
      } else if (*p == 't') {
        if (!append_char(&out, &len, &cap, '\t'))
          goto fail;
      } else if (!append_char(&out, &len, &cap, *p)) {
        goto fail;
      }
    } else if (!append_char(&out, &len, &cap, *p)) {
      goto fail;
    }
  }
fail:
  free(out);
  return NULL;
}

static void add_loaded_entry(char *owned_text) {
  if (!owned_text || !owned_text[0]) {
    free(owned_text);
    return;
  }
  if (g_count > 0 && strcmp(g_entries[g_count - 1], owned_text) == 0) {
    free(owned_text);
    return;
  }
  if (g_count == INPUT_HISTORY_LIMIT) {
    free(g_entries[0]);
    memmove(g_entries, g_entries + 1,
            sizeof(g_entries[0]) * (INPUT_HISTORY_LIMIT - 1));
    g_count--;
  }
  g_entries[g_count++] = owned_text;
}

static int persist_history(void) {
  if (!g_path[0])
    return 0;
  FILE *f = fopen(g_path, "wb");
  if (!f)
    return 0;
  for (int i = 0; i < g_count; i++) {
    char *escaped = json_escape_string(g_entries[i]);
    if (!escaped) {
      fclose(f);
      return 0;
    }
    fputs(escaped, f);
    fputc('\n', f);
    free(escaped);
  }
  fclose(f);
  return 1;
}

int input_history_load(const char *workdir) {
  input_history_reset();
  if (!set_history_path(workdir))
    return 0;

  FILE *f = fopen(g_path, "rb");
  if (!f)
    return 1;

  char line[INPUT_BUFFER_SIZE + 128];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    char *decoded = json_unescape_line(line);
    add_loaded_entry(decoded);
  }
  fclose(f);
  return 1;
}

int input_history_add(const char *text) {
  if (!text || !text[0])
    return 1;
  reset_browse();
  if (g_count > 0 && strcmp(g_entries[g_count - 1], text) == 0)
    return 1;
  char *copy = dup_string(text);
  if (!copy)
    return 0;
  add_loaded_entry(copy);
  return persist_history();
}

const char *input_history_prev(const char *current_input) {
  if (g_count == 0)
    return current_input ? current_input : "";
  if (g_browse_index < 0) {
    free(g_draft);
    g_draft = dup_string(current_input ? current_input : "");
    g_browse_index = g_count - 1;
  } else if (g_browse_index > 0) {
    g_browse_index--;
  }
  return g_entries[g_browse_index];
}

const char *input_history_next(const char *current_input) {
  (void)current_input;
  if (g_browse_index < 0)
    return current_input ? current_input : "";
  if (g_browse_index < g_count - 1) {
    g_browse_index++;
    return g_entries[g_browse_index];
  }
  g_browse_index = -1;
  const char *draft = g_draft ? g_draft : "";
  return draft;
}

const char *input_history_path(void) { return g_path; }

int input_history_count(void) { return g_count; }

const char *input_history_entry(int index) {
  if (index < 0 || index >= g_count)
    return NULL;
  return g_entries[index];
}
