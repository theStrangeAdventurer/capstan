#include "skills.h"
#include "utils.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  char *name;
  char *frontmatter_name;
  char *frontmatter_description;
  char *path;
  char *source;
  char *resources;
} SkillDef;

typedef struct {
  SkillDef *items;
  size_t count;
  size_t capacity;
} SkillList;

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} StringList;

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *read_file(const char *path) {
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
  return buf;
}

static char *copy_content(const char *content, size_t content_size) {
  char *copy = malloc(content_size + 1);
  if (!copy)
    return NULL;
  memcpy(copy, content, content_size);
  copy[content_size] = '\0';
  return copy;
}

static char *trim_copy(const char *start, size_t len) {
  while (len > 0 && (*start == ' ' || *start == '\t')) {
    start++;
    len--;
  }
  while (len > 0 &&
         (start[len - 1] == ' ' || start[len - 1] == '\t' ||
          start[len - 1] == '\r')) {
    len--;
  }
  if (len >= 2 &&
      ((start[0] == '"' && start[len - 1] == '"') ||
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

static int frontmatter_field_matches(const char *line, size_t line_len,
                                     const char *field, size_t *value_start) {
  size_t field_len = strlen(field);
  if (line_len <= field_len || strncmp(line, field, field_len) != 0)
    return 0;
  if (line[field_len] != ':')
    return 0;
  *value_start = field_len + 1;
  return 1;
}

static void parse_frontmatter(const char *content,
                              char **out_frontmatter_name,
                              char **out_frontmatter_description) {
  *out_frontmatter_name = NULL;
  *out_frontmatter_description = NULL;

  if (!content || strncmp(content, "---", 3) != 0)
    return;
  const char *p = content + 3;
  if (*p == '\r')
    p++;
  if (*p != '\n')
    return;
  p++;

  while (*p) {
    const char *line = p;
    const char *newline = strchr(p, '\n');
    size_t line_len = newline ? (size_t)(newline - line) : strlen(line);
    if (line_len > 0 && line[line_len - 1] == '\r')
      line_len--;

    if (line_len == 3 && strncmp(line, "---", 3) == 0)
      return;

    size_t value_start = 0;
    if (!*out_frontmatter_name &&
        frontmatter_field_matches(line, line_len, "name", &value_start)) {
      *out_frontmatter_name =
          trim_copy(line + value_start, line_len - value_start);
    } else if (!*out_frontmatter_description &&
               frontmatter_field_matches(line, line_len, "description",
                                         &value_start)) {
      *out_frontmatter_description =
          trim_copy(line + value_start, line_len - value_start);
    }

    if (!newline)
      return;
    p = newline + 1;
  }
}

static void skill_free(SkillDef *skill) {
  free(skill->name);
  free(skill->frontmatter_name);
  free(skill->frontmatter_description);
  free(skill->path);
  free(skill->source);
  free(skill->resources);
}

static int find_skill_index(SkillList *list, const char *name) {
  for (size_t i = 0; i < list->count; i++) {
    if (strcmp(list->items[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

static int skill_list_add(SkillList *list, const char *name, const char *path,
                          const char *source, char *frontmatter_name,
                          char *frontmatter_description, char *resources) {
  char *name_copy = my_strdup(name);
  char *path_copy = my_strdup(path);
  char *source_copy = my_strdup(source);
  if (!name_copy || !path_copy || !source_copy) {
    free(name_copy);
    free(path_copy);
    free(source_copy);
    return 0;
  }

  int existing = find_skill_index(list, name);
  if (existing >= 0) {
    skill_free(&list->items[existing]);
    list->items[existing].name = name_copy;
    list->items[existing].path = path_copy;
    list->items[existing].source = source_copy;
    list->items[existing].frontmatter_name = frontmatter_name;
    list->items[existing].frontmatter_description = frontmatter_description;
    list->items[existing].resources = resources;
    return 1;
  }

  if (list->count >= list->capacity) {
    size_t new_capacity = list->capacity ? list->capacity * 2 : 8;
    SkillDef *new_items =
        realloc(list->items, new_capacity * sizeof(SkillDef));
    if (!new_items) {
      free(name_copy);
      free(path_copy);
      free(source_copy);
      return 0;
    }
    list->items = new_items;
    list->capacity = new_capacity;
  }

  SkillDef *skill = &list->items[list->count++];
  skill->name = name_copy;
  skill->path = path_copy;
  skill->source = source_copy;
  skill->frontmatter_name = frontmatter_name;
  skill->frontmatter_description = frontmatter_description;
  skill->resources = resources;
  return 1;
}

static void skill_list_free(SkillList *list) {
  for (size_t i = 0; i < list->count; i++)
    skill_free(&list->items[i]);
  free(list->items);
}

static int path_join(char *buf, size_t buf_size, const char *dir,
                     const char *name) {
  int n = snprintf(buf, buf_size, "%s/%s", dir, name);
  return n > 0 && (size_t)n < buf_size;
}

static int string_list_add(StringList *list, const char *value) {
  if (list->count >= list->capacity) {
    size_t new_capacity = list->capacity ? list->capacity * 2 : 16;
    char **new_items = realloc(list->items, new_capacity * sizeof(char *));
    if (!new_items)
      return 0;
    list->items = new_items;
    list->capacity = new_capacity;
  }

  list->items[list->count] = my_strdup(value);
  if (!list->items[list->count])
    return 0;
  list->count++;
  return 1;
}

static void string_list_free(StringList *list) {
  for (size_t i = 0; i < list->count; i++)
    free(list->items[i]);
  free(list->items);
}

static int compare_strings(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

static int append_text(char **buf, size_t *len, size_t *capacity,
                       const char *text);

static void collect_resources(StringList *resources, const char *skill_dir,
                              const char *relative_dir) {
  char current_dir[PATH_MAX];
  if (relative_dir[0]) {
    if (!path_join(current_dir, sizeof(current_dir), skill_dir, relative_dir))
      return;
  } else {
    snprintf(current_dir, sizeof(current_dir), "%s", skill_dir);
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
    if (!path_join(full_path, sizeof(full_path), skill_dir, relative_path))
      continue;

    if (is_dir(full_path)) {
      collect_resources(resources, skill_dir, relative_path);
    } else if (is_regular_file(full_path) &&
               strcmp(relative_path, "SKILL.md") != 0) {
      string_list_add(resources, relative_path);
    }
  }

  closedir(dir);
}

static char *build_resource_manifest(const char *skill_dir) {
  StringList resources = {0};
  collect_resources(&resources, skill_dir, "");
  if (resources.count == 0) {
    string_list_free(&resources);
    return my_strdup("");
  }

  qsort(resources.items, resources.count, sizeof(char *), compare_strings);

  char *buf = NULL;
  size_t len = 0;
  size_t capacity = 0;
  for (size_t i = 0; i < resources.count; i++) {
    if (!append_text(&buf, &len, &capacity, "- ") ||
        !append_text(&buf, &len, &capacity, resources.items[i]) ||
        !append_text(&buf, &len, &capacity, "\n")) {
      free(buf);
      string_list_free(&resources);
      return my_strdup("");
    }
  }

  string_list_free(&resources);
  return buf;
}

static void scan_skill_dir(SkillList *list, const char *dir_path,
                           const char *source) {
  DIR *dir = opendir(dir_path);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;

    char entry_path[PATH_MAX];
    if (!path_join(entry_path, sizeof(entry_path), dir_path, entry->d_name))
      continue;

    if (!is_dir(entry_path))
      continue;

    char skill_path[PATH_MAX];
    if (!path_join(skill_path, sizeof(skill_path), entry_path, "SKILL.md") ||
        !is_regular_file(skill_path))
      continue;

    char *content = read_file(skill_path);
    char *resources = build_resource_manifest(entry_path);
    if (content) {
      char *frontmatter_name = NULL;
      char *frontmatter_description = NULL;
      parse_frontmatter(content, &frontmatter_name, &frontmatter_description);
      if (!frontmatter_name)
        frontmatter_name = my_strdup(entry->d_name);
      if (!frontmatter_description)
        frontmatter_description = my_strdup("");
      if (!skill_list_add(list, entry->d_name, skill_path, source,
                          frontmatter_name, frontmatter_description,
                          resources)) {
        free(frontmatter_name);
        free(frontmatter_description);
        free(resources);
      }
      free(content);
    } else {
      free(resources);
    }
  }

  closedir(dir);
}

static void scan_builtin_skills(SkillList *list,
                                const BuiltinSkill *builtin_skills,
                                size_t builtin_skill_count) {
  if (!builtin_skills)
    return;

  for (size_t i = 0; i < builtin_skill_count; i++) {
    const BuiltinSkill *builtin = &builtin_skills[i];
    if (!builtin->name || !builtin->path || !builtin->content)
      continue;

    char *content = copy_content(builtin->content, builtin->content_size);
    if (!content)
      continue;

    char *frontmatter_name = NULL;
    char *frontmatter_description = NULL;
    parse_frontmatter(content, &frontmatter_name, &frontmatter_description);
    if (!frontmatter_name)
      frontmatter_name = my_strdup(builtin->name);
    if (!frontmatter_description)
      frontmatter_description = my_strdup("");

    char *resources = my_strdup("");
    if (!skill_list_add(list, builtin->name, builtin->path, "builtin",
                        frontmatter_name, frontmatter_description,
                        resources)) {
      free(frontmatter_name);
      free(frontmatter_description);
      free(resources);
    }
    free(content);
  }
}

static int compare_skills(const void *a, const void *b) {
  const SkillDef *sa = (const SkillDef *)a;
  const SkillDef *sb = (const SkillDef *)b;
  int name_cmp = strcmp(sa->name, sb->name);
  if (name_cmp != 0)
    return name_cmp;
  return strcmp(sa->source, sb->source);
}

static void dirname_copy(char *out, size_t out_size, const char *path) {
  snprintf(out, out_size, "%s", path);
  char *slash = strrchr(out, '/');
  if (!slash)
    snprintf(out, out_size, ".");
  else if (slash == out)
    out[1] = '\0';
  else
    *slash = '\0';
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

char *skills_build_prompt(const BuiltinSkill *builtin_skills,
                          size_t builtin_skill_count,
                          const char *project_skills_dir,
                          const char *user_skills_dir,
                          const char *common_skills_dir) {
  SkillList list = {0};

  scan_builtin_skills(&list, builtin_skills, builtin_skill_count);
  if (common_skills_dir)
    scan_skill_dir(&list, common_skills_dir, "common");
  if (user_skills_dir)
    scan_skill_dir(&list, user_skills_dir, "user");
  if (project_skills_dir)
    scan_skill_dir(&list, project_skills_dir, "project");

  if (list.count == 0) {
    skill_list_free(&list);
    return my_strdup("");
  }

  qsort(list.items, list.count, sizeof(SkillDef), compare_skills);

  char *buf = NULL;
  size_t len = 0;
  size_t capacity = 0;
  if (!append_text(&buf, &len, &capacity,
                   "\n\n# Skills\n"
                   "The following skill index was loaded from "
                   "`skills/name/SKILL.md` under built-in gated skills, "
                   "project `.agents/skills/`, `~/.agents/skills/`, and "
                   "`~/.config/capstan/skills/`. Only "
                   "FrontMatter metadata is included here.\n"
                   "Mandatory skill use rule: if the user names a skill, or if "
                   "the task matches a skill description, you must read that "
                   "skill's `Skill file` path completely before using the "
                   "skill. A matching skill has priority zero: use it before "
                   "MCP tools, built-in tools, fetch/direct HTTP, and shell. "
                   "If the loaded skill explicitly prescribes a tool or "
                   "command path, including shell/curl, follow the skill's "
                   "tool instructions. When delegating skill-based work to "
                   "subagents, read the skill in the orchestrator first, pass "
                   "the concrete workflow/tool instructions through the "
                   "subagents instructions field, and restrict each subagent "
                   "to the narrow required tools. "
                   "Do not apply a skill from this index alone.\n")) {
    skill_list_free(&list);
    return my_strdup("");
  }

  for (size_t i = 0; i < list.count; i++) {
    char header[PATH_MAX + 512];
    snprintf(header, sizeof(header),
             "\n## Skill: %s\nName: %s\nDescription: %s\nSource: %s\nSkill "
             "file: %s\n",
             list.items[i].name,
             list.items[i].frontmatter_name ? list.items[i].frontmatter_name
                                             : list.items[i].name,
             list.items[i].frontmatter_description
                 ? list.items[i].frontmatter_description
                 : "",
             list.items[i].source, list.items[i].path);

    if (!append_text(&buf, &len, &capacity, header))
      goto fail;
  }

  skill_list_free(&list);
  return buf;

fail:
  free(buf);
  skill_list_free(&list);
  return my_strdup("");
}

char *skills_build_summary(const BuiltinSkill *builtin_skills,
                           size_t builtin_skill_count,
                           const char *project_skills_dir,
                           const char *user_skills_dir,
                           const char *common_skills_dir) {
  SkillList list = {0};

  scan_builtin_skills(&list, builtin_skills, builtin_skill_count);
  if (common_skills_dir)
    scan_skill_dir(&list, common_skills_dir, "common");
  if (user_skills_dir)
    scan_skill_dir(&list, user_skills_dir, "user");
  if (project_skills_dir)
    scan_skill_dir(&list, project_skills_dir, "project");

  if (list.count == 0) {
    skill_list_free(&list);
    return my_strdup("No skills loaded.");
  }

  qsort(list.items, list.count, sizeof(SkillDef), compare_skills);

  char *buf = NULL;
  size_t len = 0;
  size_t capacity = 0;
  char header[64];
  snprintf(header, sizeof(header), "Loaded skills: %zu\n", list.count);
  if (!append_text(&buf, &len, &capacity, header))
    goto fail;

  for (size_t i = 0; i < list.count; i++) {
    char skill_dir[PATH_MAX];
    dirname_copy(skill_dir, sizeof(skill_dir), list.items[i].path);

    char item_header[(PATH_MAX * 2) + 160];
    snprintf(item_header, sizeof(item_header),
             "\n- %s [%s]\n  Name: %s\n  Description: %s\n  SKILL.md: %s\n  "
             "Resource root: %s\n",
             list.items[i].name, list.items[i].source,
             list.items[i].frontmatter_name ? list.items[i].frontmatter_name
                                             : list.items[i].name,
             list.items[i].frontmatter_description
                 ? list.items[i].frontmatter_description
                 : "",
             list.items[i].path, skill_dir);
    if (!append_text(&buf, &len, &capacity, item_header))
      goto fail;

    if (list.items[i].resources && list.items[i].resources[0]) {
      if (!append_text(&buf, &len, &capacity, "  Resources:\n"))
        goto fail;
      char *resources = my_strdup(list.items[i].resources);
      if (!resources)
        goto fail;
      char *line = strtok(resources, "\n");
      while (line) {
        if (!append_text(&buf, &len, &capacity, "    ") ||
            !append_text(&buf, &len, &capacity, line) ||
            !append_text(&buf, &len, &capacity, "\n")) {
          free(resources);
          goto fail;
        }
        line = strtok(NULL, "\n");
      }
      free(resources);
    }
  }

  skill_list_free(&list);
  return buf;

fail:
  free(buf);
  skill_list_free(&list);
  return my_strdup("No skills loaded.");
}
