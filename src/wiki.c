#include "wiki.h"
#include "utils.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  char *relative_path;
  char *source_id;
  char *source_path;
  char *id;
  char *kind;
  char *title;
  char *description;
  char *use_when;
  char *tags;
  char *index_policy;
  char *context_policy;
} WikiDoc;

typedef struct {
  WikiDoc *items;
  size_t count;
  size_t capacity;
} WikiList;

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (n != (size_t)size) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  if (out_size)
    *out_size = n;
  return buf;
}

char *wiki_expand_path(const char *path) {
  if (!path || !path[0])
    return NULL;
  if (path[0] != '~')
    return my_strdup(path);
  if (path[1] != '\0' && path[1] != '/')
    return my_strdup(path);
  const char *home = getenv("HOME");
  if (!home || !home[0])
    return my_strdup(path);
  size_t home_len = strlen(home);
  size_t rest_len = strlen(path + 1);
  char *result = malloc(home_len + rest_len + 1);
  if (!result)
    return NULL;
  memcpy(result, home, home_len);
  memcpy(result + home_len, path + 1, rest_len + 1);
  return result;
}

static char *trim_copy(const char *start, size_t len) {
  while (len > 0 && (*start == ' ' || *start == '\t')) {
    start++;
    len--;
  }
  while (len > 0 &&
         (start[len - 1] == ' ' || start[len - 1] == '\t' ||
          start[len - 1] == '\r'))
    len--;
  if (len >= 2 && ((start[0] == '"' && start[len - 1] == '"') ||
                   (start[0] == '\'' && start[len - 1] == '\''))) {
    start++;
    len -= 2;
  }
  char *copy = malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, start, len);
  copy[len] = '\0';
  return copy;
}

static int field_matches(const char *line, size_t line_len, const char *field,
                         size_t *value_start) {
  size_t field_len = strlen(field);
  if (line_len <= field_len || strncmp(line, field, field_len) != 0)
    return 0;
  if (line[field_len] != ':')
    return 0;
  *value_start = field_len + 1;
  return 1;
}

static int append_text(char **buf, size_t *len, size_t *capacity,
                       const char *text) {
  size_t text_len = strlen(text);
  if (*len + text_len + 1 > *capacity) {
    size_t new_capacity = *capacity ? *capacity : 1024;
    while (*len + text_len + 1 > new_capacity)
      new_capacity *= 2;
    char *new_buf = realloc(*buf, new_capacity);
    if (!new_buf)
      return 0;
    *buf = new_buf;
    *capacity = new_capacity;
  }
  memcpy(*buf + *len, text, text_len);
  *len += text_len;
  (*buf)[*len] = '\0';
  return 1;
}

static void append_multivalue(char **target, const char *value) {
  if (!value)
    return;
  if (!*target) {
    *target = my_strdup(value);
    return;
  }
  size_t old_len = strlen(*target);
  size_t value_len = strlen(value);
  char *next = realloc(*target, old_len + value_len + 3);
  if (!next)
    return;
  next[old_len] = ';';
  next[old_len + 1] = ' ';
  memcpy(next + old_len + 2, value, value_len + 1);
  *target = next;
}

static char **doc_field(WikiDoc *doc, const char *field) {
  if (strcmp(field, "id") == 0)
    return &doc->id;
  if (strcmp(field, "kind") == 0)
    return &doc->kind;
  if (strcmp(field, "title") == 0)
    return &doc->title;
  if (strcmp(field, "description") == 0)
    return &doc->description;
  if (strcmp(field, "index_policy") == 0)
    return &doc->index_policy;
  if (strcmp(field, "context_policy") == 0)
    return &doc->context_policy;
  return NULL;
}

static int parse_frontmatter(const char *content, WikiDoc *doc) {
  if (!content || strncmp(content, "---", 3) != 0)
    return 0;
  const char *p = content + 3;
  if (*p == '\r')
    p++;
  if (*p != '\n')
    return 0;
  p++;
  char active_list[32] = "";
  while (*p) {
    const char *line = p;
    const char *newline = strchr(p, '\n');
    size_t line_len = newline ? (size_t)(newline - line) : strlen(line);
    if (line_len > 0 && line[line_len - 1] == '\r')
      line_len--;
    if (line_len == 3 && strncmp(line, "---", 3) == 0)
      return 1;

    if ((line_len > 2 && line[0] == ' ' && line[1] == ' ' && line[2] == '-') ||
        (line_len > 0 && line[0] == '-')) {
      const char *value = line[0] == '-' ? line + 1 : line + 3;
      size_t value_len = line_len - (size_t)(value - line);
      char *copy = trim_copy(value, value_len);
      if (copy) {
        if (strcmp(active_list, "use_when") == 0)
          append_multivalue(&doc->use_when, copy);
        else if (strcmp(active_list, "tags") == 0)
          append_multivalue(&doc->tags, copy);
        free(copy);
      }
    } else {
      active_list[0] = '\0';
      size_t value_start = 0;
      const char *fields[] = {"id", "kind", "title", "description",
                              "index_policy", "context_policy"};
      for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (field_matches(line, line_len, fields[i], &value_start)) {
          char **slot = doc_field(doc, fields[i]);
          if (slot && !*slot)
            *slot = trim_copy(line + value_start, line_len - value_start);
          break;
        }
      }
      if (field_matches(line, line_len, "use_when", &value_start)) {
        snprintf(active_list, sizeof(active_list), "use_when");
        if (line_len > value_start) {
          char *copy = trim_copy(line + value_start, line_len - value_start);
          append_multivalue(&doc->use_when, copy);
          free(copy);
        }
      } else if (field_matches(line, line_len, "tags", &value_start)) {
        snprintf(active_list, sizeof(active_list), "tags");
        if (line_len > value_start) {
          char *copy = trim_copy(line + value_start, line_len - value_start);
          append_multivalue(&doc->tags, copy);
          free(copy);
        }
      }
    }
    if (!newline)
      return 0;
    p = newline + 1;
  }
  return 0;
}

static void wiki_doc_free(WikiDoc *doc) {
  free(doc->relative_path);
  free(doc->source_id);
  free(doc->source_path);
  free(doc->id);
  free(doc->kind);
  free(doc->title);
  free(doc->description);
  free(doc->use_when);
  free(doc->tags);
  free(doc->index_policy);
  free(doc->context_policy);
}

static void wiki_list_free(WikiList *list) {
  for (size_t i = 0; i < list->count; i++)
    wiki_doc_free(&list->items[i]);
  free(list->items);
}

static int wiki_list_add(WikiList *list, WikiDoc *doc) {
  if (list->count >= list->capacity) {
    size_t new_capacity = list->capacity ? list->capacity * 2 : 16;
    WikiDoc *new_items = realloc(list->items, new_capacity * sizeof(WikiDoc));
    if (!new_items)
      return 0;
    list->items = new_items;
    list->capacity = new_capacity;
  }
  list->items[list->count++] = *doc;
  memset(doc, 0, sizeof(*doc));
  return 1;
}

static int path_join(char *buf, size_t buf_size, const char *dir,
                     const char *name) {
  int n = snprintf(buf, buf_size, "%s/%s", dir, name);
  return n > 0 && (size_t)n < buf_size;
}

static int is_markdown_file(const char *name) {
  size_t len = name ? strlen(name) : 0;
  return (len > 3 && strcmp(name + len - 3, ".md") == 0) ||
         (len > 9 && strcmp(name + len - 9, ".markdown") == 0);
}

static int is_json_file(const char *name) {
  size_t len = name ? strlen(name) : 0;
  return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

static const char *json_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    p++;
  return p;
}

static char *json_parse_string(const char **cursor, const char *end) {
  const char *p = json_skip_ws(*cursor, end);
  if (p >= end || *p != '"')
    return NULL;
  p++;
  char *buf = malloc((size_t)(end - p) + 1);
  if (!buf)
    return NULL;
  size_t len = 0;
  while (p < end) {
    unsigned char c = (unsigned char)*p++;
    if (c == '"') {
      buf[len] = '\0';
      *cursor = p;
      return buf;
    }
    if (c == '\\' && p < end) {
      char esc = *p++;
      switch (esc) {
      case '"':
      case '\\':
      case '/':
        buf[len++] = esc;
        break;
      case 'b':
        buf[len++] = '\b';
        break;
      case 'f':
        buf[len++] = '\f';
        break;
      case 'n':
        buf[len++] = '\n';
        break;
      case 'r':
        buf[len++] = '\r';
        break;
      case 't':
        buf[len++] = '\t';
        break;
      case 'u':
        /* Keep unicode escapes ASCII-safe; generated indexes normally use UTF-8. */
        if ((size_t)(end - p) >= 4)
          p += 4;
        buf[len++] = '?';
        break;
      default:
        buf[len++] = esc;
        break;
      }
    } else {
      buf[len++] = (char)c;
    }
  }
  free(buf);
  return NULL;
}

static const char *json_find_key(const char *start, const char *end,
                                 const char *key) {
  char pattern[128];
  int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  if (n <= 0 || (size_t)n >= sizeof(pattern))
    return NULL;
  const char *p = start;
  while (p && p < end) {
    const char *found = strstr(p, pattern);
    if (!found || found >= end)
      return NULL;
    p = found + strlen(pattern);
    p = json_skip_ws(p, end);
    if (p < end && *p == ':')
      return p + 1;
  }
  return NULL;
}

static char *json_string_field(const char *start, const char *end,
                               const char *key) {
  const char *p = json_find_key(start, end, key);
  if (!p)
    return NULL;
  return json_parse_string(&p, end);
}

static char *json_string_array_field(const char *start, const char *end,
                                     const char *key) {
  const char *p = json_find_key(start, end, key);
  if (!p)
    return NULL;
  p = json_skip_ws(p, end);
  if (p >= end || *p != '[')
    return NULL;
  p++;
  char *joined = NULL;
  while (p < end) {
    p = json_skip_ws(p, end);
    if (p >= end || *p == ']')
      break;
    char *value = json_parse_string(&p, end);
    if (value) {
      append_multivalue(&joined, value);
      free(value);
    }
    p = json_skip_ws(p, end);
    if (p < end && *p == ',')
      p++;
  }
  return joined;
}

static const char *json_matching_brace(const char *open, const char *end) {
  if (!open || open >= end || *open != '{')
    return NULL;
  int depth = 0;
  int in_string = 0;
  int escaped = 0;
  for (const char *p = open; p < end; p++) {
    char c = *p;
    if (in_string) {
      if (escaped) {
        escaped = 0;
      } else if (c == '\\') {
        escaped = 1;
      } else if (c == '"') {
        in_string = 0;
      }
      continue;
    }
    if (c == '"') {
      in_string = 1;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0)
        return p + 1;
    }
  }
  return NULL;
}

static void collect_docs(WikiList *list, const char *root,
                         const char *relative_dir) {
  char current_dir[PATH_MAX];
  if (relative_dir[0]) {
    if (!path_join(current_dir, sizeof(current_dir), root, relative_dir))
      return;
  } else {
    snprintf(current_dir, sizeof(current_dir), "%s", root);
  }
  DIR *dir = opendir(current_dir);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    char relative_path[PATH_MAX];
    if (relative_dir[0]) {
      if (!path_join(relative_path, sizeof(relative_path), relative_dir,
                     entry->d_name))
        continue;
    } else {
      snprintf(relative_path, sizeof(relative_path), "%s", entry->d_name);
    }
    char full_path[PATH_MAX];
    if (!path_join(full_path, sizeof(full_path), root, relative_path))
      continue;
    if (is_dir(full_path)) {
      collect_docs(list, root, relative_path);
    } else if (is_regular_file(full_path) && is_markdown_file(entry->d_name) &&
               strcmp(relative_path, "profile/core.md") != 0) {
      size_t size = 0;
      char *content = read_file(full_path, &size);
      if (!content)
        continue;
      WikiDoc doc = {0};
      if (parse_frontmatter(content, &doc)) {
        doc.relative_path = my_strdup(relative_path);
        if (!doc.title)
          doc.title = my_strdup(relative_path);
        if (!wiki_list_add(list, &doc))
          wiki_doc_free(&doc);
      }
      free(content);
    }
  }
  closedir(dir);
}

static void collect_index_file(WikiList *list, const char *full_path) {
  size_t size = 0;
  char *content = read_file(full_path, &size);
  if (!content)
    return;
  const char *start = content;
  const char *end = content + size;
  char *source_id = json_string_field(start, end, "source_id");
  char *source_root = json_string_field(start, end, "source_root");
  const char *entries = json_find_key(start, end, "entries");
  if (!source_id || !source_root || !entries) {
    free(source_id);
    free(source_root);
    free(content);
    return;
  }
  entries = json_skip_ws(entries, end);
  if (entries >= end || *entries != '[') {
    free(source_id);
    free(source_root);
    free(content);
    return;
  }
  const char *p = entries + 1;
  while (p < end) {
    p = json_skip_ws(p, end);
    if (p >= end || *p == ']')
      break;
    if (*p != '{') {
      p++;
      continue;
    }
    const char *obj_end = json_matching_brace(p, end);
    if (!obj_end)
      break;
    WikiDoc doc = {0};
    doc.source_id = my_strdup(source_id);
    doc.source_path = json_string_field(p, obj_end, "path");
    doc.id = json_string_field(p, obj_end, "id");
    doc.kind = json_string_field(p, obj_end, "kind");
    doc.title = json_string_field(p, obj_end, "title");
    doc.description = json_string_field(p, obj_end, "description");
    doc.use_when = json_string_array_field(p, obj_end, "use_when");
    doc.tags = json_string_array_field(p, obj_end, "tags");
    doc.index_policy = json_string_field(p, obj_end, "index_policy");
    doc.context_policy = json_string_field(p, obj_end, "context_policy");
    if (doc.source_id && doc.source_path && doc.source_path[0]) {
      size_t rel_len = strlen(doc.source_id) + strlen(doc.source_path) + 9;
      doc.relative_path = malloc(rel_len);
      if (doc.relative_path) {
        snprintf(doc.relative_path, rel_len, "source:%s:%s", doc.source_id,
                 doc.source_path);
      }
      if (!doc.title)
        doc.title = my_strdup(doc.source_path);
      if (!doc.kind)
        doc.kind = my_strdup("source");
      if (!doc.context_policy)
        doc.context_policy = my_strdup("retrieve_only");
      if (!wiki_list_add(list, &doc))
        wiki_doc_free(&doc);
    } else {
      wiki_doc_free(&doc);
    }
    p = obj_end;
  }
  free(source_id);
  free(source_root);
  free(content);
}

static void collect_index_docs(WikiList *list, const char *root) {
  char index_dir[PATH_MAX];
  if (!path_join(index_dir, sizeof(index_dir), root, "index"))
    return;
  DIR *dir = opendir(index_dir);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.' || !is_json_file(entry->d_name))
      continue;
    char full_path[PATH_MAX];
    if (!path_join(full_path, sizeof(full_path), index_dir, entry->d_name))
      continue;
    if (is_regular_file(full_path))
      collect_index_file(list, full_path);
  }
  closedir(dir);
}

static int compare_docs(const void *a, const void *b) {
  const WikiDoc *da = (const WikiDoc *)a;
  const WikiDoc *db = (const WikiDoc *)b;
  return strcmp(da->relative_path, db->relative_path);
}

static char *configured_path_or_status(const char *wiki_path, int *ok) {
  *ok = 0;
  char *expanded = wiki_expand_path(wiki_path);
  if (!expanded || !expanded[0]) {
    free(expanded);
    return my_strdup("Wiki is not configured. Set wiki.path in ~/.config/capstan/config.lua.");
  }
  if (!is_dir(expanded)) {
    char msg[PATH_MAX + 128];
    snprintf(msg, sizeof(msg), "Wiki path is not a directory: %s", expanded);
    free(expanded);
    return my_strdup(msg);
  }
  *ok = 1;
  return expanded;
}

char *wiki_build_prompt(const char *wiki_path) {
  int ok = 0;
  char *root = configured_path_or_status(wiki_path, &ok);
  if (!ok)
    return my_strdup("");

  char *core = NULL;
  size_t core_size = 0;
  char core_path[PATH_MAX];
  if (path_join(core_path, sizeof(core_path), root, "profile/core.md"))
    core = read_file(core_path, &core_size);

  WikiList list = {0};
  collect_docs(&list, root, "");
  collect_index_docs(&list, root);
  qsort(list.items, list.count, sizeof(WikiDoc), compare_docs);

  char *buf = NULL;
  size_t len = 0;
  size_t capacity = 0;
  append_text(&buf, &len, &capacity, "\n\n# Wiki\n");
  append_text(&buf, &len, &capacity, "The following owner wiki context was loaded. profile/core.md is included fully when present. Indexed wiki documents are metadata-only; read full wiki documents only when useful with the `wiki_read` tool or `/wiki read <relative-path>`. Indexed external sources are also metadata-only; read them only when useful with the `wiki_source_read` tool using the listed source and path. Wiki matching is advisory, not mandatory.\n");
  append_text(&buf, &len, &capacity, "Wiki path: ");
  append_text(&buf, &len, &capacity, root);
  append_text(&buf, &len, &capacity, "\n");
  if (core) {
    append_text(&buf, &len, &capacity, "\n## profile/core.md\n");
    append_text(&buf, &len, &capacity, core);
    if (core_size == 0 || core[core_size - 1] != '\n')
      append_text(&buf, &len, &capacity, "\n");
  }
  if (list.count > 0) {
    append_text(&buf, &len, &capacity, "\n## Wiki index\n");
    for (size_t i = 0; i < list.count; i++) {
      WikiDoc *doc = &list.items[i];
      if (doc->source_id) {
        char header[PATH_MAX + 768];
        snprintf(header, sizeof(header),
                 "\n### %s\nPath: %s\nSource: %s\nSource path: %s\nRead with: wiki_source_read {\"source\":\"%s\",\"path\":\"%s\"}\nID: %s\nKind: %s\nTitle: %s\nDescription: %s\nUse when: %s\nTags: %s\nIndex policy: %s\nContext policy: %s\n",
                 doc->title ? doc->title : doc->relative_path,
                 doc->relative_path ? doc->relative_path : "",
                 doc->source_id ? doc->source_id : "",
                 doc->source_path ? doc->source_path : "",
                 doc->source_id ? doc->source_id : "",
                 doc->source_path ? doc->source_path : "",
                 doc->id ? doc->id : "", doc->kind ? doc->kind : "",
                 doc->title ? doc->title : "",
                 doc->description ? doc->description : "",
                 doc->use_when ? doc->use_when : "",
                 doc->tags ? doc->tags : "",
                 doc->index_policy ? doc->index_policy : "",
                 doc->context_policy ? doc->context_policy : "");
        append_text(&buf, &len, &capacity, header);
      } else {
        char header[PATH_MAX + 512];
        snprintf(header, sizeof(header),
                 "\n### %s\nPath: %s\nID: %s\nKind: %s\nTitle: %s\nDescription: %s\nUse when: %s\nTags: %s\nIndex policy: %s\nContext policy: %s\n",
                 doc->title ? doc->title : doc->relative_path,
                 doc->relative_path, doc->id ? doc->id : "",
                 doc->kind ? doc->kind : "", doc->title ? doc->title : "",
                 doc->description ? doc->description : "",
                 doc->use_when ? doc->use_when : "",
                 doc->tags ? doc->tags : "",
                 doc->index_policy ? doc->index_policy : "",
                 doc->context_policy ? doc->context_policy : "");
        append_text(&buf, &len, &capacity, header);
      }
    }
  }

  free(core);
  wiki_list_free(&list);
  free(root);
  return buf ? buf : my_strdup("");
}

char *wiki_build_summary(const char *wiki_path) {
  int ok = 0;
  char *root = configured_path_or_status(wiki_path, &ok);
  if (!ok)
    return root;
  WikiList list = {0};
  collect_docs(&list, root, "");
  collect_index_docs(&list, root);
  qsort(list.items, list.count, sizeof(WikiDoc), compare_docs);
  char core_path[PATH_MAX];
  int has_core = path_join(core_path, sizeof(core_path), root, "profile/core.md") &&
                 is_regular_file(core_path);

  char *buf = NULL;
  size_t len = 0;
  size_t capacity = 0;
  char header[PATH_MAX + 128];
  snprintf(header, sizeof(header), "Wiki path: %s\nProfile core: %s\nIndexed documents: %zu\n",
           root, has_core ? "loaded" : "missing", list.count);
  append_text(&buf, &len, &capacity, header);
  for (size_t i = 0; i < list.count; i++) {
    WikiDoc *doc = &list.items[i];
    char item[PATH_MAX + 768];
    if (doc->source_id) {
      snprintf(item, sizeof(item),
               "\n- %s\n  Source: %s\n  Source path: %s\n  Title: %s\n  Description: %s\n  Tags: %s\n",
               doc->relative_path, doc->source_id ? doc->source_id : "",
               doc->source_path ? doc->source_path : "",
               doc->title ? doc->title : "",
               doc->description ? doc->description : "",
               doc->tags ? doc->tags : "");
    } else {
      snprintf(item, sizeof(item),
               "\n- %s\n  Title: %s\n  Description: %s\n  Tags: %s\n",
               doc->relative_path, doc->title ? doc->title : "",
               doc->description ? doc->description : "",
               doc->tags ? doc->tags : "");
    }
    append_text(&buf, &len, &capacity, item);
  }
  wiki_list_free(&list);
  free(root);
  return buf ? buf : my_strdup("");
}
