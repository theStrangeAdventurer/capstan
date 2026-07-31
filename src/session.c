#include "session.h"
#include "app_config.h"
#include "utils.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SESSION_VERSION 1
#define SESSION_MAX_MESSAGES 10000
#define SESSION_MAX_LINE (4 * 1024 * 1024)

static char g_store_dir[PATH_MAX] = "";

static char *dup_string(const char *text) {
  size_t len = strlen(text ? text : "");
  char *copy = malloc(len + 1);
  if (copy)
    memcpy(copy, text ? text : "", len + 1);
  return copy;
}

unsigned long session_workspace_hash(const char *text) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;
       p++) {
    h ^= (uint64_t)*p;
    h *= UINT64_C(1099511628211);
  }
  return (unsigned long)h;
}

static int ensure_dir(const char *path, mode_t mode) {
  if (mkdir(path, mode) != 0 && errno != EEXIST)
    return 0;
  chmod(path, mode);
  return 1;
}

int session_store_init(const char *workspace_root) {
  g_store_dir[0] = '\0';
  if (app_state_ensure_dir() != 0)
    return 0;
  char state[PATH_MAX], sessions[PATH_MAX];
  if (app_state_dir(state, sizeof(state)) != 0)
    return 0;
  int n = snprintf(sessions, sizeof(sessions), "%s/sessions", state);
  if (n < 0 || (size_t)n >= sizeof(sessions) || !ensure_dir(sessions, 0700))
    return 0;
  n = snprintf(g_store_dir, sizeof(g_store_dir), "%s/%016lx", sessions,
               session_workspace_hash(workspace_root));
  if (n < 0 || (size_t)n >= sizeof(g_store_dir) ||
      !ensure_dir(g_store_dir, 0700)) {
    g_store_dir[0] = '\0';
    return 0;
  }
  return 1;
}

const char *session_store_dir(void) { return g_store_dir; }

static int valid_utf8_sequence(const unsigned char *p, size_t remaining,
                               size_t *length) {
  if (*p < 0x80) {
    *length = 1;
    return 1;
  }
  size_t needed;
  if (*p >= 0xc2 && *p <= 0xdf)
    needed = 2;
  else if (*p >= 0xe0 && *p <= 0xef)
    needed = 3;
  else if (*p >= 0xf0 && *p <= 0xf4)
    needed = 4;
  else
    return 0;
  if (remaining < needed)
    return 0;
  for (size_t i = 1; i < needed; i++)
    if ((p[i] & 0xc0) != 0x80)
      return 0;
  if ((*p == 0xe0 && p[1] < 0xa0) ||
      (*p == 0xed && p[1] >= 0xa0) ||
      (*p == 0xf0 && p[1] < 0x90) ||
      (*p == 0xf4 && p[1] >= 0x90))
    return 0;
  *length = needed;
  return 1;
}

int session_id_valid(const char *id) {
  if (!id || !id[0] || strlen(id) >= SESSION_ID_SIZE)
    return 0;
  size_t len = strlen(id);
  if (id[0] == ' ' || id[len - 1] == ' ' || id[0] == '.')
    return 0;
  const unsigned char *p = (const unsigned char *)id;
  size_t remaining = len;
  while (remaining > 0) {
    if (*p < 0x80 &&
        (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\'))
      return 0;
    size_t sequence = 0;
    if (!valid_utf8_sequence(p, remaining, &sequence))
      return 0;
    p += sequence;
    remaining -= sequence;
  }
  return 1;
}

static int session_path(const char *id, char *path, size_t path_size) {
  if (!g_store_dir[0] || !session_id_valid(id))
    return 0;
  int n = snprintf(path, path_size, "%s/%s.jsonl", g_store_dir, id);
  return n >= 0 && (size_t)n < path_size;
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

static char *json_escape(const char *text) {
  char *out = NULL;
  size_t len = 0, cap = 0;
  if (!append_char(&out, &len, &cap, '"'))
    return NULL;
  for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;
       p++) {
    const char *escape = NULL;
    if (*p == '"') escape = "\\\"";
    else if (*p == '\\') escape = "\\\\";
    else if (*p == '\n') escape = "\\n";
    else if (*p == '\r') escape = "\\r";
    else if (*p == '\t') escape = "\\t";
    if (escape) {
      for (const char *q = escape; *q; q++)
        if (!append_char(&out, &len, &cap, *q)) goto fail;
    } else if (*p < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
      for (const char *q = escaped; *q; q++)
        if (!append_char(&out, &len, &cap, *q)) goto fail;
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

static int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static char *json_field_string(const char *line, const char *field) {
  char needle[128];
  int n = snprintf(needle, sizeof(needle), "\"%s\":\"", field);
  if (n < 0 || (size_t)n >= sizeof(needle))
    return NULL;
  const char *p = strstr(line, needle);
  if (!p)
    return NULL;
  p += strlen(needle);
  char *out = NULL;
  size_t len = 0, cap = 0;
  while (*p && *p != '"') {
    if (*p != '\\') {
      if (!append_char(&out, &len, &cap, *p++)) goto fail;
      continue;
    }
    p++;
    if (!*p) goto fail;
    char decoded = *p;
    if (*p == 'n') decoded = '\n';
    else if (*p == 'r') decoded = '\r';
    else if (*p == 't') decoded = '\t';
    else if (*p == 'u') {
      int value = 0;
      for (int i = 1; i <= 4; i++) {
        int h = hex_value(p[i]);
        if (h < 0) goto fail;
        value = value * 16 + h;
      }
      decoded = value <= 0x7f ? (char)value : '?';
      p += 4;
    }
    if (!append_char(&out, &len, &cap, decoded)) goto fail;
    p++;
  }
  if (*p != '"') goto fail;
  return out ? out : dup_string("");
fail:
  free(out);
  return NULL;
}

static long long json_field_integer(const char *line, const char *field,
                                    long long fallback) {
  char needle[128];
  int n = snprintf(needle, sizeof(needle), "\"%s\":", field);
  if (n < 0 || (size_t)n >= sizeof(needle))
    return fallback;
  const char *p = strstr(line, needle);
  if (!p)
    return fallback;
  char *end = NULL;
  long long value = strtoll(p + strlen(needle), &end, 10);
  return end == p + strlen(needle) ? fallback : value;
}

void session_title_from_text(const char *text, char *title, size_t title_size) {
  if (!title || title_size == 0)
    return;
  char normalized[SESSION_TITLE_SIZE * 4];
  size_t out = 0;
  int space = 0;
  text = text ? text : "";
  while (*text && out + 1 < sizeof(normalized)) {
    unsigned char ch = (unsigned char)*text++;
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
      space = out > 0;
      continue;
    }
    if (space && out + 1 < sizeof(normalized))
      normalized[out++] = ' ';
    space = 0;
    normalized[out++] = (char)ch;
  }
  normalized[out] = '\0';
  if (!normalized[0]) {
    snprintf(title, title_size, "New session");
    return;
  }
  utf8_truncate(normalized, title, title_size, 48, "…");
}

int session_create(Session *session) {
  if (!session || !g_store_dir[0])
    return 0;
  memset(session, 0, sizeof(*session));
  struct timeval tv;
  gettimeofday(&tv, NULL);
  static unsigned long sequence = 0;
  sequence++;
  snprintf(session->id, sizeof(session->id), "%08lx-%08lx-%04lx",
           (unsigned long)tv.tv_sec, (unsigned long)tv.tv_usec,
           (unsigned long)((getpid() + sequence) & 0xffff));
  snprintf(session->title, sizeof(session->title), "New session");
  session->created_at = tv.tv_sec;
  session->updated_at = tv.tv_sec;
  return session_save(session) && session_set_active(session->id);
}

int session_create_named(Session *session, const char *id) {
  if (!session || !g_store_dir[0] || !session_id_valid(id))
    return 0;
  memset(session, 0, sizeof(*session));
  char path[PATH_MAX];
  if (!session_path(id, path, sizeof(path)))
    return 0;
  int reserved = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (reserved < 0)
    return 0;
  if (close(reserved) != 0) {
    unlink(path);
    return 0;
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);
  snprintf(session->id, sizeof(session->id), "%s", id);
  snprintf(session->title, sizeof(session->title), "%s", id);
  session->title_generated = 1;
  session->created_at = tv.tv_sec;
  session->updated_at = tv.tv_sec;
  if (session_save(session))
    return 1;
  unlink(path);
  memset(session, 0, sizeof(*session));
  return 0;
}

int session_save(const Session *session) {
  if (!session || !session_id_valid(session->id))
    return 0;
  char path[PATH_MAX], temp[PATH_MAX];
  if (!session_path(session->id, path, sizeof(path)))
    return 0;
  int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp))
    return 0;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return 0;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(temp);
    return 0;
  }
  char *id = json_escape(session->id);
  char *title = json_escape(session->title);
  int ok = id && title &&
           fprintf(f, "{\"version\":%d,\"id\":%s,\"title\":%s,"
                      "\"title_generated\":%d,\"created_at\":%lld,\"updated_at\":%lld}\n",
                   SESSION_VERSION, id, title, session->title_generated,
                   (long long)session->created_at,
                   (long long)session->updated_at) >= 0;
  free(id);
  free(title);
  for (size_t i = 0; ok && i < session->message_count; i++) {
    const SessionMessage *message = &session->messages[i];
    if (!message->text || !message->text[0])
      continue;
    char *text = json_escape(message->text);
    char *raw = json_escape(message->raw_text ? message->raw_text : message->text);
    ok = text && raw &&
         fprintf(f, "{\"role\":\"%s\",\"text\":%s,\"raw_text\":%s}\n",
                 message->role == SESSION_ROLE_USER ? "user" : "assistant",
                 text, raw) >= 0;
    free(text);
    free(raw);
  }
  if (ok && fflush(f) == 0)
    ok = fsync(fd) == 0;
  if (fclose(f) != 0)
    ok = 0;
  if (ok)
    ok = rename(temp, path) == 0;
  if (!ok)
    unlink(temp);
  if (ok)
    chmod(path, 0600);
  return ok;
}

typedef enum {
  SESSION_LINE_OK,
  SESSION_LINE_EOF,
  SESSION_LINE_ERROR,
} SessionLineStatus;

static char *read_line(FILE *f, SessionLineStatus *status) {
  if (status)
    *status = SESSION_LINE_ERROR;
  char *line = NULL;
  size_t len = 0, cap = 0;
  int ch;
  while ((ch = fgetc(f)) != EOF) {
    if (ch == '\n')
      break;
    if (len >= SESSION_MAX_LINE) {
      free(line);
      return NULL;
    }
    if (!append_char(&line, &len, &cap, (char)ch)) {
      free(line);
      return NULL;
    }
  }
  if (ch == EOF && ferror(f)) {
    free(line);
    return NULL;
  }
  if (ch == EOF && len == 0) {
    free(line);
    if (status)
      *status = SESSION_LINE_EOF;
    return NULL;
  }
  if (!line)
    line = dup_string("");
  if (!line)
    return NULL;
  if (status)
    *status = SESSION_LINE_OK;
  return line;
}

void session_free(Session *session) {
  if (!session)
    return;
  for (size_t i = 0; i < session->message_count; i++) {
    free(session->messages[i].text);
    free(session->messages[i].raw_text);
  }
  free(session->messages);
  memset(session, 0, sizeof(*session));
}

int session_load(const char *id, Session *session) {
  if (!session)
    return 0;
  memset(session, 0, sizeof(*session));
  char path[PATH_MAX];
  if (!session_path(id, path, sizeof(path)))
    return 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  SessionLineStatus line_status = SESSION_LINE_ERROR;
  char *line = read_line(f, &line_status);
  int ok = line_status == SESSION_LINE_OK && line &&
           json_field_integer(line, "version", -1) == SESSION_VERSION;
  char *stored_id = ok ? json_field_string(line, "id") : NULL;
  char *title = ok ? json_field_string(line, "title") : NULL;
  if (!stored_id || strcmp(stored_id, id) != 0 || !title)
    ok = 0;
  if (ok) {
    snprintf(session->id, sizeof(session->id), "%s", stored_id);
    snprintf(session->title, sizeof(session->title), "%s", title);
    session->title_generated =
        (int)json_field_integer(line, "title_generated", 0);
    session->created_at = (time_t)json_field_integer(line, "created_at", 0);
    session->updated_at = (time_t)json_field_integer(line, "updated_at", 0);
  }
  free(stored_id);
  free(title);
  free(line);
  while (ok) {
    line = read_line(f, &line_status);
    if (line_status == SESSION_LINE_EOF)
      break;
    if (line_status != SESSION_LINE_OK || !line) {
      ok = 0;
      break;
    }
    if (!line[0]) { free(line); continue; }
    char *role = json_field_string(line, "role");
    char *text = json_field_string(line, "text");
    char *raw = json_field_string(line, "raw_text");
    if (!role || !text || !raw ||
        (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0) ||
        session->message_count >= SESSION_MAX_MESSAGES) {
      free(role); free(text); free(raw); free(line); ok = 0; break;
    }
    SessionMessage *grown = realloc(
        session->messages,
        (session->message_count + 1) * sizeof(SessionMessage));
    if (!grown) {
      free(role); free(text); free(raw); free(line); ok = 0; break;
    }
    session->messages = grown;
    SessionMessage *message = &session->messages[session->message_count++];
    message->role = strcmp(role, "user") == 0 ? SESSION_ROLE_USER
                                               : SESSION_ROLE_ASSISTANT;
    message->text = text;
    message->raw_text = raw;
    free(role);
    free(line);
  }
  fclose(f);
  if (!ok)
    session_free(session);
  return ok;
}

int session_delete(const char *id) {
  char path[PATH_MAX];
  return session_path(id, path, sizeof(path)) && unlink(path) == 0;
}

static int compare_info(const void *a, const void *b) {
  const SessionInfo *left = a;
  const SessionInfo *right = b;
  if (left->updated_at < right->updated_at) return 1;
  if (left->updated_at > right->updated_at) return -1;
  return strcmp(left->id, right->id);
}

int session_list(SessionInfo **items, size_t *count) {
  if (!items || !count || !g_store_dir[0])
    return 0;
  *items = NULL;
  *count = 0;
  DIR *dir = opendir(g_store_dir);
  if (!dir)
    return errno == ENOENT;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len <= 6 || strcmp(entry->d_name + len - 6, ".jsonl") != 0)
      continue;
    char id[SESSION_ID_SIZE];
    if (len - 6 >= sizeof(id))
      continue;
    memcpy(id, entry->d_name, len - 6);
    id[len - 6] = '\0';
    Session loaded;
    if (!session_load(id, &loaded))
      continue;
    SessionInfo *grown = realloc(*items, (*count + 1) * sizeof(SessionInfo));
    if (!grown) { session_free(&loaded); closedir(dir); free(*items); *items = NULL; *count = 0; return 0; }
    *items = grown;
    SessionInfo *info = &(*items)[(*count)++];
    snprintf(info->id, sizeof(info->id), "%s", loaded.id);
    snprintf(info->title, sizeof(info->title), "%s", loaded.title);
    info->title_generated = loaded.title_generated;
    info->created_at = loaded.created_at;
    info->updated_at = loaded.updated_at;
    session_free(&loaded);
  }
  closedir(dir);
  qsort(*items, *count, sizeof(SessionInfo), compare_info);
  return 1;
}

void session_list_free(SessionInfo *items) { free(items); }

static int active_path(char *path, size_t path_size) {
  if (!g_store_dir[0]) return 0;
  int n = snprintf(path, path_size, "%s/active", g_store_dir);
  return n >= 0 && (size_t)n < path_size;
}

int session_set_active(const char *id) {
  if (!session_id_valid(id)) return 0;
  char path[PATH_MAX], temp[PATH_MAX];
  if (!active_path(path, sizeof(path))) return 0;
  int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return 0;
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return 0;
  size_t len = strlen(id);
  int ok = write(fd, id, len) == (ssize_t)len && write(fd, "\n", 1) == 1 &&
           fsync(fd) == 0;
  if (close(fd) != 0)
    ok = 0;
  if (ok)
    ok = rename(temp, path) == 0;
  if (!ok)
    unlink(temp);
  if (ok) chmod(path, 0600);
  return ok;
}

int session_get_active(char *id, size_t id_size) {
  if (!id || id_size == 0) return 0;
  id[0] = '\0';
  char path[PATH_MAX];
  if (!active_path(path, sizeof(path))) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  int ok = fgets(id, (int)id_size, f) != NULL;
  fclose(f);
  if (!ok) return 0;
  id[strcspn(id, "\r\n")] = '\0';
  if (!session_id_valid(id)) { id[0] = '\0'; return 0; }
  return 1;
}
