#include "project_instructions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int regular_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_file(const char *path, size_t *size_out) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long end = ftell(file);
  if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size_t size = (size_t)end;
  char *content = malloc(size + 1);
  if (!content) {
    fclose(file);
    return NULL;
  }
  size_t read_size = fread(content, 1, size, file);
  fclose(file);
  if (read_size != size) {
    free(content);
    return NULL;
  }
  content[size] = '\0';
  if (size_out)
    *size_out = size;
  return content;
}

static int append_source(char **prompt, size_t *used, size_t *capacity,
                         const char *title, const char *path) {
  size_t content_size = 0;
  char *content = read_file(path, &content_size);
  if (!content)
    return 1;

  const char *prefix = "\n\n# ";
  const char *path_prefix = "\nPath: ";
  const char *separator = "\n\n";
  size_t needed = strlen(prefix) + strlen(title) + strlen(path_prefix) +
                  strlen(path) + strlen(separator) + content_size;
  if (*used + needed + 1 > *capacity) {
    size_t next = *capacity ? *capacity : 1024;
    while (next < *used + needed + 1)
      next *= 2;
    char *grown = realloc(*prompt, next);
    if (!grown) {
      free(content);
      return 0;
    }
    *prompt = grown;
    *capacity = next;
  }

  int written = snprintf(*prompt + *used, *capacity - *used,
                         "%s%s%s%s%s", prefix, title, path_prefix, path,
                         separator);
  if (written < 0) {
    free(content);
    return 0;
  }
  *used += (size_t)written;
  memcpy(*prompt + *used, content, content_size);
  *used += content_size;
  (*prompt)[*used] = '\0';
  free(content);
  return 1;
}

char *project_instructions_build_prompt(const char *workspace_root,
                                        const char *config_dir,
                                        const char *home_dir) {
  char config_agents[4096];
  char shared_agents[4096];
  char project_agents[4096];
  const char *user_path = NULL;

  if (config_dir) {
    int n = snprintf(config_agents, sizeof(config_agents), "%s/AGENTS.md",
                     config_dir);
    if (n > 0 && (size_t)n < sizeof(config_agents) &&
        regular_file(config_agents))
      user_path = config_agents;
  }
  if (!user_path && home_dir) {
    int n = snprintf(shared_agents, sizeof(shared_agents),
                     "%s/.agents/AGENTS.md", home_dir);
    if (n > 0 && (size_t)n < sizeof(shared_agents) &&
        regular_file(shared_agents))
      user_path = shared_agents;
  }

  const char *project_path = NULL;
  if (workspace_root) {
    int n = snprintf(project_agents, sizeof(project_agents), "%s/AGENTS.md",
                     workspace_root);
    if (n > 0 && (size_t)n < sizeof(project_agents) &&
        regular_file(project_agents))
      project_path = project_agents;
  }

  char *prompt = NULL;
  size_t used = 0;
  size_t capacity = 0;
  if (user_path &&
      !append_source(&prompt, &used, &capacity, "User Instructions", user_path)) {
    free(prompt);
    return NULL;
  }
  if (project_path && (!user_path || strcmp(project_path, user_path) != 0) &&
      !append_source(&prompt, &used, &capacity, "Project Instructions",
                     project_path)) {
    free(prompt);
    return NULL;
  }
  return prompt;
}
