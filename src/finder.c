#include "finder.h"
#include "utils.h"
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FINDER_MAX_FILES 20000

typedef struct {
  PopupItem *items;
  int count;
  int capacity;
} FinderItems;

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int path_join(char *buf, size_t buf_size, const char *dir,
                     const char *name) {
  int n = snprintf(buf, buf_size, "%s/%s", dir, name);
  return n > 0 && (size_t)n < buf_size;
}

static char *trim_line(char *line) {
  while (*line == ' ' || *line == '\t')
    line++;
  size_t len = strlen(line);
  while (len > 0 &&
         (line[len - 1] == '\n' || line[len - 1] == '\r' ||
          line[len - 1] == ' ' || line[len - 1] == '\t')) {
    line[--len] = '\0';
  }
  return line;
}

void finder_ignore_init(FinderIgnoreList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

void finder_ignore_free(FinderIgnoreList *list) {
  for (int i = 0; i < list->count; i++)
    free(list->items[i].pattern);
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

int finder_ignore_add(FinderIgnoreList *list, const char *pattern) {
  if (!pattern)
    return 1;

  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s", pattern);
  char *p = trim_line(buf);
  if (!p[0] || p[0] == '#')
    return 1;

  int negated = 0;
  if (*p == '!') {
    negated = 1;
    p++;
  }

  int anchored = 0;
  if (*p == '/') {
    anchored = 1;
    p++;
  }

  size_t len = strlen(p);
  int dir_only = 0;
  while (len > 0 && p[len - 1] == '/') {
    dir_only = 1;
    p[--len] = '\0';
  }
  if (len == 0)
    return 1;

  if (list->count >= list->capacity) {
    int new_capacity = list->capacity ? list->capacity * 2 : 16;
    FinderIgnorePattern *items =
        realloc(list->items, new_capacity * sizeof(FinderIgnorePattern));
    if (!items)
      return 0;
    list->items = items;
    list->capacity = new_capacity;
  }

  FinderIgnorePattern *item = &list->items[list->count++];
  item->pattern = my_strdup(p);
  if (!item->pattern)
    return 0;
  item->negated = negated;
  item->anchored = anchored;
  item->dir_only = dir_only;
  item->has_slash = strchr(p, '/') != NULL;
  return 1;
}

int finder_ignore_load_file(FinderIgnoreList *list, const char *root,
                            const char *filename) {
  char path[PATH_MAX];
  if (!path_join(path, sizeof(path), root, filename))
    return 0;

  FILE *f = fopen(path, "r");
  if (!f)
    return 1;

  char line[PATH_MAX];
  while (fgets(line, sizeof(line), f)) {
    if (!finder_ignore_add(list, line)) {
      fclose(f);
      return 0;
    }
  }
  fclose(f);
  return 1;
}

static int pattern_matches(const FinderIgnorePattern *pat,
                           const char *rel_path, int is_dir) {
  if (pat->dir_only && !is_dir)
    return 0;

  const char *rel = rel_path;
  if (pat->anchored || pat->has_slash)
    return fnmatch(pat->pattern, rel, 0) == 0;

  if (fnmatch(pat->pattern, base_name(rel), 0) == 0)
    return 1;

  char with_slash[PATH_MAX];
  int n = snprintf(with_slash, sizeof(with_slash), "*/%s", pat->pattern);
  return n > 0 && (size_t)n < sizeof(with_slash) &&
         fnmatch(with_slash, rel, 0) == 0;
}

int finder_path_ignored(const FinderIgnoreList *list, const char *rel_path,
                        int is_dir) {
  int ignored = 0;
  if (strcmp(rel_path, ".git") == 0 || strncmp(rel_path, ".git/", 5) == 0)
    ignored = 1;

  for (int i = 0; i < list->count; i++) {
    if (pattern_matches(&list->items[i], rel_path, is_dir))
      ignored = !list->items[i].negated;
  }
  return ignored;
}

static int finder_items_add(FinderItems *items, const char *text,
                            const char *value) {
  if (items->count >= FINDER_MAX_FILES)
    return 1;
  if (items->count >= items->capacity) {
    int new_capacity = items->capacity ? items->capacity * 2 : 128;
    PopupItem *new_items = realloc(items->items, new_capacity * sizeof(PopupItem));
    if (!new_items)
      return 0;
    items->items = new_items;
    items->capacity = new_capacity;
  }
  items->items[items->count].text = my_strdup(text);
  items->items[items->count].value = my_strdup(value);
  if (!items->items[items->count].text || !items->items[items->count].value)
    return 0;
  items->count++;
  return 1;
}

static int scan_dir(const char *root, const char *rel_dir,
                    const FinderIgnoreList *ignore, FinderItems *items) {
  char abs_dir[PATH_MAX];
  if (rel_dir[0]) {
    if (!path_join(abs_dir, sizeof(abs_dir), root, rel_dir))
      return 0;
  } else {
    snprintf(abs_dir, sizeof(abs_dir), "%s", root);
  }

  DIR *dir = opendir(abs_dir);
  if (!dir)
    return 1;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char rel_path[PATH_MAX];
    if (rel_dir[0]) {
      if (!path_join(rel_path, sizeof(rel_path), rel_dir, entry->d_name))
        continue;
    } else {
      snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
    }

    char abs_path[PATH_MAX];
    if (!path_join(abs_path, sizeof(abs_path), root, rel_path))
      continue;

    struct stat st;
    if (lstat(abs_path, &st) != 0)
      continue;
    int is_dir = S_ISDIR(st.st_mode);
    if (finder_path_ignored(ignore, rel_path, is_dir))
      continue;

    if (is_dir) {
      if (!scan_dir(root, rel_path, ignore, items)) {
        closedir(dir);
        return 0;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (!finder_items_add(items, rel_path, abs_path)) {
        closedir(dir);
        return 0;
      }
    }
  }

  closedir(dir);
  return 1;
}

int finder_collect_files(const char *root, const FinderIgnoreList *ignore,
                         PopupItem **out_items, int *out_count) {
  *out_items = NULL;
  *out_count = 0;
  FinderItems items = {0};
  if (!scan_dir(root, "", ignore, &items)) {
    for (int i = 0; i < items.count; i++) {
      free(items.items[i].text);
      free(items.items[i].value);
    }
    free(items.items);
    return 0;
  }
  *out_items = items.items;
  *out_count = items.count;
  return 1;
}

static int fuzzy_score_inner(const char *text, const char *query) {
  if (!query || !query[0])
    return 1;
  if (!text)
    return 0;

  int score = 0;
  int ti = 0;
  int last = -1;
  for (int qi = 0; query[qi]; qi++) {
    unsigned char qc = (unsigned char)query[qi];
    if (qc >= 'A' && qc <= 'Z')
      qc = (unsigned char)(qc - 'A' + 'a');
    int found = -1;
    for (; text[ti]; ti++) {
      unsigned char tc = (unsigned char)text[ti];
      if (tc >= 'A' && tc <= 'Z')
        tc = (unsigned char)(tc - 'A' + 'a');
      if (tc == qc) {
        found = ti++;
        break;
      }
    }
    if (found < 0)
      return 0;
    score += 10;
    if (last >= 0 && found == last + 1)
      score += 5;
    if (found == 0 || text[found - 1] == '/' || text[found - 1] == '_' ||
        text[found - 1] == '-' || text[found - 1] == '.')
      score += 3;
    last = found;
  }

  score -= (int)strlen(text) / 8;
  return score > 0 ? score : 1;
}

int finder_fuzzy_score(const char *text, const char *query) {
  int score = fuzzy_score_inner(text, query);
  if (score <= 0)
    return 0;

  const char *base = base_name(text);
  if (base != text && fuzzy_score_inner(base, query) > 0)
    score += 20;
  return score;
}
