#include "background_work.h"
#include "redact.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define RECORDS 128
#define OUTPUT_LIMIT 65536

typedef struct {
  BackgroundSnapshot s;
  char status[16];
  char *output;
  int cancelled, notify;
} Work;
static Work records[RECORDS];
static size_t count;
static unsigned long long serial;
static Work *find(const char *id) {
  if (id) for (size_t i = 0; i < count; i++)
    if (!strcmp(id, records[i].s.id)) return &records[i];
  return NULL;
}
/* Strip terminal escape sequences before redaction, then impose public bounds.
 * Never retain raw model output in this registry. */
static char *safe_text(const char *text) {
  size_t n = strlen(text);
  char *clean = malloc(n + 1);
  if (!clean) return NULL;
  size_t used = 0;
  int state = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (state == 1) { state = c == '[' ? 2 : (c == ']' || c == 'P' || c == '^' || c == '_') ? 3 : 0; continue; }
    if (state == 2) { if (c >= 0x40 && c <= 0x7e) state = 0; continue; }
    if (state == 3) { if (c == 7) state = 0; else if (c == 27) state = 4; continue; }
    if (state == 4) { state = c == '\\' ? 0 : 3; continue; }
    if (c == 27) { state = 1; continue; }
    if ((c < 32 && c != '\n' && c != '\t') || c == 127) continue;
    clean[used++] = (char)c;
  }
  clean[used] = 0;
  char *safe = redact_secrets_alloc(clean);
  free(clean);
  return safe;
}
static int copy_safe(char *dst, size_t n, const char *src) {
  char *safe = safe_text(src ? src : "");
  if (!safe) return 0;
  snprintf(dst, n, "%s", safe);
  free(safe);
  return 1;
}
int background_work_register(const char *owner, const char *kind, const char *label,
                             const char *workdir, char id[PROCESS_ID_SIZE]) {
  if (!id || !owner || strlen(owner) >= 128 || !kind ||
      (strcmp(kind, "subagent") && strcmp(kind, "subagent_group")) || serial == UINT64_MAX) return 0;
  unsigned char entropy[16];
  FILE *random = fopen("/dev/urandom", "rb");
  if (!random) return 0;
  size_t got = fread(entropy, 1, sizeof(entropy), random);
  fclose(random);
  if (got != sizeof(entropy)) return 0;
  char nonce[33];
  for (size_t i = 0; i < sizeof(entropy); i++) snprintf(nonce + i * 2, 3, "%02x", entropy[i]);
  Work w = {0};
  if (!copy_safe(w.s.owner, sizeof(w.s.owner), owner) ||
      !copy_safe(w.s.label, sizeof(w.s.label), label) ||
      !copy_safe(w.s.workdir, sizeof(w.s.workdir), workdir)) return 0;
  /* A terminal child may still belong to an active group. Without explicit
   * group reclamation, never evict records implicitly. */
  if (count == RECORDS) return 0;
  snprintf(w.s.id, sizeof(w.s.id), "background-%s-%llu", nonce, ++serial);
  snprintf(w.s.kind, sizeof(w.s.kind), "%s", kind);
  strcpy(w.status, "queued");
  w.s.running = 1; w.s.exit_code = -1;
  w.s.started_ms = process_manager_now_ms();
  w.notify = !strcmp(kind, "subagent_group");
  records[count++] = w;
  memcpy(id, w.s.id, PROCESS_ID_SIZE);
  return 1;
}
int background_work_update(const char *id, const char *status, const char *output, int ok) {
  Work *w = find(id);
  if (!w || !w->s.running) return 0;
  if (status && strcmp(status, "queued") && strcmp(status, "running") &&
      strcmp(status, "completed") && strcmp(status, "failed") && strcmp(status, "cancelled")) return 0;
  if (output) {
    char *safe = safe_text(output);
    if (!safe) return 0;
    size_t n = strlen(safe);
    /* Reject atomically: truncating serialized group results corrupts JSON. */
    if (n > OUTPUT_LIMIT) { free(safe); return 0; }
    free(w->output); w->output = safe;
    w->s.output_available = 1;
  }
  if (status && !strcmp(status, "cancelled")) {
    w->cancelled = w->s.stopping = 1;
    if (!strcmp(w->s.kind, "subagent")) process_manager_stop_task(w->s.id);
  }
  if (status) snprintf(w->status, sizeof(w->status), "%s", status);
  w->s.running = !strcmp(w->status, "queued") || !strcmp(w->status, "running");
  if (!w->s.running) {
    w->s.finished_ms = process_manager_now_ms();
    w->s.exit_code = ok ? 0 : 1;
  }
  return 1;
}
int background_work_cancelled(const char *id) { Work *w = find(id); return !w || w->cancelled; }
int background_work_inprocess(const BackgroundSnapshot *s) {
  return !strcmp(s->kind, "subagent") || !strcmp(s->kind, "subagent_group");
}
const char *background_work_status(const BackgroundSnapshot *s) {
  Work *w = find(s->id);
  if (w) return w->status;
  return s->running ? (s->stopping ? "stopping" : "running") : s->timed_out ? "timed_out" : "exited";
}
size_t background_work_count(void) { return process_manager_count() + count; }
int background_work_at(size_t i, BackgroundSnapshot *s) {
  size_t os = process_manager_count();
  if (i < os) return process_manager_at(i, s);
  if (!s || i - os >= count) return 0;
  *s = records[i - os].s; return 1;
}
int background_work_get(const char *id, BackgroundSnapshot *s) {
  Work *w = find(id);
  if (!w) return process_manager_get(id, s);
  if (!s) return 0;
  *s = w->s; return 1;
}
int background_work_stop(const char *id) {
  Work *w = find(id);
  if (!w) return process_manager_stop(id);
  if (!w->s.running) return 0;
  w->cancelled = w->s.stopping = 1;
  if (!strcmp(w->s.kind, "subagent")) process_manager_stop_task(w->s.id);
  return 1;
}
char *background_work_output(const char *id, int stream) {
  Work *w = find(id);
  if (!w) return process_manager_output(id, stream);
  const char *text = !stream && w->output ? w->output : "";
  char *result = malloc(strlen(text) + 1);
  if (result) strcpy(result, text);
  return result;
}
int background_work_completion(const char *owner, BackgroundSnapshot *s) {
  if (!owner || !s) return 0;
  if (process_manager_completion(owner, s)) return 1;
  for (size_t i = 0; i < count; i++) {
    Work *w = &records[i];
    if (w->notify && !w->s.running && !strcmp(owner, w->s.owner)) {
      w->notify = 0; *s = w->s; return 1;
    }
  }
  return 0;
}
size_t background_work_cancel_owner(const char *owner) {
  size_t n = 0;
  for (size_t i = 0; i < count; i++)
    if (records[i].s.running && (!owner || !strcmp(owner, records[i].s.owner))) {
      if (!records[i].cancelled && strcmp(records[i].s.kind, "subagent_group")) n++;
      background_work_stop(records[i].s.id);
    }
  return n;
}
size_t background_work_cancel_all(void) { return background_work_cancel_owner(NULL); }
void background_work_shutdown(void) {
  background_work_cancel_all();
  for (size_t i = 0; i < count; i++) free(records[i].output);
  count = 0;
  process_manager_shutdown();
}
