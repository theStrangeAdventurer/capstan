/* libproc requires Darwin extensions even with the project's POSIX baseline. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "process_observe.h"
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <dirent.h>
#include <stdio.h>
#include <limits.h>
#elif defined(__APPLE__)
#include <libproc.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#define OBS_MAX 65536
#define OBS_DEPTH 256

typedef struct {
  ProcessDescendant value;
  unsigned long long born, born_fraction;
} Observation;

static void clean_name(char *dst, const char *src, size_t len) {
  size_t i = 0;
  while (i < len && i < 127 && src[i]) {
    unsigned char c = (unsigned char)src[i];
    dst[i++] = c >= 32 && c < 127 ? (char)c : '?';
  }
  dst[i] = '\0';
}

static int read_process(pid_t pid, Observation *o) {
  memset(o, 0, sizeof(*o));
#if defined(__linux__)
  char path[64], buf[4096];
  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  int failed = ferror(f);
  fclose(f);
  if (failed || !n || n == sizeof(buf) - 1) return 0;
  buf[n] = '\0';
  char *left = strchr(buf, '('), *right = strrchr(buf, ')');
  if (!left || !right || right <= left || right[1] != ' ') return 0;
  char *end;
  long actual = strtol(buf, &end, 10);
  if (actual != pid || end != left - 1) return 0;
  clean_name(o->value.name, left + 1, (size_t)(right - left - 1));
  char *save = NULL;
  char *token = strtok_r(right + 2, " ", &save);
  for (int field = 3; field <= 22; field++) {
    if (!token) return 0;
    if (field == 4 || field == 5 || field == 22) {
      unsigned long long v = strtoull(token, &end, 10);
      if (end == token || *end) return 0;
      if (field != 22 && v > INT_MAX) return 0;
      if (field == 4) o->value.ppid = (pid_t)v;
      if (field == 5) o->value.pgid = (pid_t)v;
      if (field == 22) o->born = v;
    }
    token = strtok_r(NULL, " ", &save);
  }
#else
  struct proc_bsdinfo info;
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
    return 0;
  if ((pid_t)info.pbi_pid != pid) return 0;
  o->value.ppid = (pid_t)info.pbi_ppid;
  o->value.pgid = (pid_t)info.pbi_pgid;
  o->born = info.pbi_start_tvsec;
  o->born_fraction = info.pbi_start_tvusec;
  clean_name(o->value.name, info.pbi_comm, sizeof(info.pbi_comm));
#endif
  o->value.pid = pid;
  return 1;
}

static size_t snapshot(Observation *rows) {
  size_t count = 0;
#if defined(__linux__)
  DIR *dir = opendir("/proc");
  if (!dir) return 0;
  struct dirent *entry;
  size_t scanned = 0;
  while (count < OBS_MAX && scanned++ < OBS_MAX * 2 &&
         (entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
    char *end;
    long pid = strtol(entry->d_name, &end, 10);
    if (*end || pid <= 0 || pid > INT_MAX) continue;
    if (read_process((pid_t)pid, &rows[count])) count++;
  }
  closedir(dir);
#else
  pid_t *pids = malloc(OBS_MAX * sizeof(*pids));
  if (!pids) return 0;
  int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, OBS_MAX * sizeof(*pids));
  size_t n = bytes > 0 ? (size_t)bytes / sizeof(*pids) : 0;
  if (n > OBS_MAX) n = OBS_MAX;
  for (size_t i = 0; i < n; i++)
    if (pids[i] > 0 && read_process(pids[i], &rows[count])) count++;
  free(pids);
#endif
  return count;
}

static int compare(const void *a, const void *b) {
  pid_t x = ((const Observation *)a)->value.pid;
  pid_t y = ((const Observation *)b)->value.pid;
  return (x > y) - (x < y);
}
static const Observation *find(const Observation *rows, size_t n, pid_t pid) {
  Observation key = {0};
  key.value.pid = pid;
  return bsearch(&key, rows, n, sizeof(*rows), compare);
}
static int unchanged(const Observation *o) {
  Observation now;
  return read_process(o->value.pid, &now) && now.born == o->born &&
    now.born_fraction == o->born_fraction &&
    now.value.ppid == o->value.ppid && now.value.pgid == o->value.pgid;
}
#endif

size_t process_observe_descendants(pid_t root_pid, pid_t root_pgid,
                                   ProcessDescendant *out, size_t capacity) {
#if defined(__linux__) || defined(__APPLE__)
  if (root_pid <= 0 || root_pgid <= 0 || !out || !capacity) return 0;
  Observation root;
  if (!read_process(root_pid, &root) || root.value.pgid != root_pgid) return 0;
  Observation *rows = malloc(OBS_MAX * sizeof(*rows));
  if (!rows) return 0;
  size_t n = snapshot(rows), written = 0;
  qsort(rows, n, sizeof(*rows), compare);
  if (capacity > 4096) capacity = 4096;
  for (size_t i = 0; i < n && written < capacity; i++) {
    const Observation *chain[OBS_DEPTH], *current = &rows[i];
    size_t depth = 0;
    while (current && current->value.pid != root_pid && depth < OBS_DEPTH) {
      chain[depth++] = current;
      current = find(rows, n, current->value.ppid);
    }
    if (!current || current->value.pid != root_pid || !depth) continue;
    int valid = 1;
    for (size_t j = 0; j < depth; j++)
      if (!unchanged(chain[j])) { valid = 0; break; }
    if (!valid) continue;
    out[written] = rows[i].value;
    out[written].in_managed_group = out[written].pgid == root_pgid;
    written++;
  }
  if (!unchanged(&root)) written = 0;
  free(rows);
  return written;
#else
  (void)root_pid; (void)root_pgid; (void)out; (void)capacity;
  return 0;
#endif
}
