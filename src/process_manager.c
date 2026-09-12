#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "process_manager.h"
#include "redact.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECORDS 128
#define OUTPUT_LIMIT (16U * 1024U * 1024U)
typedef struct {
  ProcessSnapshot s;
  int fd[2], lost, killed, overflow[2], notify;
  char *text[2];
  size_t len[2], cap[2];
  long long deadline, force, drain;
} Record;
static Record records[RECORDS];
static size_t count;
static unsigned long long serial;
static char current_owner[128];
static char runtime_nonce[33];
static pid_t nonce_pid;
static char *sanitize(const char *text, size_t len);
static int safe_copy(char *dst, size_t size, const char *src) {
  char *safe = sanitize(src, strlen(src));
  if (!safe) return 0;
  snprintf(dst, size, "%s", safe);
  free(safe);
  return 1;
}
/* A fresh nonce also distinguishes forked runtimes inheriting static state.
 * Fail adoption closed if OS entropy is unavailable; never fall back to a PID. */
static int ensure_nonce(void) {
  if (nonce_pid == getpid()) return 1;
  unsigned char bytes[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  size_t used = 0;
  while (used < sizeof(bytes)) {
    ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
    if (n > 0) used += (size_t)n;
    else if (n < 0 && errno == EINTR) continue;
    else { close(fd); return 0; }
  }
  close(fd);
  for (size_t i = 0; i < sizeof(bytes); i++)
    snprintf(runtime_nonce + i * 2, 3, "%02x", bytes[i]);
  nonce_pid = getpid();
  return 1;
}
static char *copy_text(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}
long long process_manager_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void process_manager_set_owner(const char *owner) {
  if (!safe_copy(current_owner, sizeof(current_owner), owner ? owner : ""))
    snprintf(current_owner, sizeof(current_owner), "[REDACTION_FAILED]");
}
const char *process_manager_owner(void) { return current_owner; }
static Record *find(const char *id) {
  if (id) for (size_t i = 0; i < count; i++)
    if (!strcmp(records[i].s.id, id)) return &records[i];
  return NULL;
}
static void close_stream(Record *r, int i) {
  if (r->fd[i] >= 0) close(r->fd[i]);
  r->fd[i] = -1;
}
static void dispose(Record *r) {
  for (int i = 0; i < 2; i++) { close_stream(r, i); free(r->text[i]); }
}
static int configure_fd(int fd) {
  if (fd < 0) return 1;
  int flags = fcntl(fd, F_GETFL);
  int fdflags = fcntl(fd, F_GETFD);
  return flags >= 0 && fdflags >= 0 &&
    fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
    fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) == 0;
}
int process_manager_adopt(pid_t pid, const char *kind, const char *label,
                          const char *workdir, int out_fd, int err_fd,
                          size_t max_stdout, size_t max_stderr, int timeout_sec,
                          char id[PROCESS_ID_SIZE]) {
  if (pid <= 0 || !id || serial == UINT64_MAX || max_stdout > OUTPUT_LIMIT ||
      max_stderr > OUTPUT_LIMIT || getpgid(pid) != pid) return 0;
  if (out_fd >= 0 && out_fd == err_fd) return 0;
  if (!ensure_nonce()) return 0;
  size_t reserved = 0;
  for (size_t i = 0; i < count; i++) {
    if (records[i].s.running && records[i].s.pid == pid) return 0;
    reserved += records[i].cap[0] + records[i].cap[1];
  }
  /* A hard aggregate budget complements the per-stream and record bounds. */
  while (count && (count == RECORDS ||
         reserved + max_stdout + max_stderr > 2U * OUTPUT_LIMIT)) {
    size_t i;
    for (i = 0; i < count && records[i].s.running; i++) {}
    if (i == count) return 0;
    reserved -= records[i].cap[0] + records[i].cap[1];
    dispose(&records[i]);
    memmove(records + i, records + i + 1, (count - i - 1) * sizeof(Record));
    count--;
  }
  Record r = {0};
  r.fd[0] = out_fd; r.fd[1] = err_fd;
  r.cap[0] = max_stdout; r.cap[1] = max_stderr;
  for (int i = 0; i < 2; i++) {
    r.text[i] = calloc(r.cap[i] + 1, 1);
    if (!r.text[i]) { free(r.text[0]); return 0; }
  }
  if (!configure_fd(out_fd) || !configure_fd(err_fd)) {
    free(r.text[0]); free(r.text[1]); return 0;
  }
  snprintf(r.s.id, sizeof(r.s.id), "process-%s-%llu", runtime_nonce, ++serial);
  if (!safe_copy(r.s.kind, sizeof(r.s.kind), kind ? kind : "process") ||
      !safe_copy(r.s.label, sizeof(r.s.label), label ? label : "") ||
      !safe_copy(r.s.workdir, sizeof(r.s.workdir), workdir ? workdir : "")) {
    free(r.text[0]); free(r.text[1]); return 0;
  }
  snprintf(r.s.owner, sizeof(r.s.owner), "%s", current_owner);
  r.s.pid = r.s.pgid = pid;
  r.s.running = 1; r.s.exit_code = -1;
  r.s.started_ms = process_manager_now_ms();
  if (timeout_sec > 0) r.deadline = r.s.started_ms + (long long)timeout_sec * 1000;
  records[count++] = r;
  memcpy(id, r.s.id, PROCESS_ID_SIZE);
  return 1;
}
static void label_char(char *label, size_t *used, char c) {
  if (*used < PROCESS_TEXT_SIZE - 1) label[(*used)++] = c;
}
int process_manager_adopt_argv(pid_t pid, const char *kind, char *const argv[],
                               const char *workdir, int out_fd, int err_fd,
                               size_t max_stdout, size_t max_stderr, int timeout_sec,
                               char id[PROCESS_ID_SIZE]) {
  if (!argv || !argv[0]) return 0;
  char label[PROCESS_TEXT_SIZE];
  size_t used = 0;
  int hide_next = 0;
  for (size_t i = 0; argv[i]; i++) {
    /* Sanitize the entire argument before bounding the display. Classify
     * options using the same secret-key policy as ordinary text redaction. */
    char *safe = sanitize(argv[i], strlen(argv[i]));
    if (!safe) return 0;
    const char *key = safe;
    while (*key == '-') key++;
    char *eq = strchr(safe, '=');
    int sensitive = i > 0 && redact_sensitive_key(key,
      eq ? (size_t)(eq - key) : strlen(key));
    const char *value = safe;
    if (hide_next) value = "[REDACTED]";
    else if (sensitive && eq) {
      /* Mask the whole argv value, including spaces and embedded quotes. */
      size_t prefix = (size_t)(eq - safe) + 1;
      char *masked = malloc(prefix + sizeof("[REDACTED]"));
      if (!masked) { free(safe); return 0; }
      memcpy(masked, safe, prefix);
      memcpy(masked + prefix, "[REDACTED]", sizeof("[REDACTED]"));
      free(safe); safe = masked; value = safe;
    }
    hide_next = !hide_next && sensitive && !eq && safe[0] == '-';
    if (i) label_char(label, &used, ' ');
    int quote = !value[0] || strpbrk(value, " \t\n\r\"'\\") != NULL;
    if (quote) label_char(label, &used, '"');
    for (const char *p = value; *p; p++) {
      if (*p == '\n' || *p == '\t' || *p == '\r') {
        label_char(label, &used, '\\');
        label_char(label, &used, *p == '\n' ? 'n' : *p == '\t' ? 't' : 'r');
      } else {
        if (*p == '"' || *p == '\\') label_char(label, &used, '\\');
        label_char(label, &used, *p);
      }
    }
    if (quote) label_char(label, &used, '"');
    free(safe);
    if (used == sizeof(label) - 1) {
      memcpy(label + used - 3, "...", 3);
      break;
    }
  }
  label[used] = 0;
  return process_manager_adopt(pid, kind, label, workdir, out_fd, err_fd,
    max_stdout, max_stderr, timeout_sec, id);
}
static int make_pipe(int p[2]) {
  if (pipe(p)) return 0;
  for (int i = 0; i < 2; i++) {
    if (p[i] <= STDERR_FILENO) {
      int fd = fcntl(p[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (fd < 0) { close(p[0]); close(p[1]); return 0; }
      close(p[i]); p[i] = fd;
    }
  }
  if (fcntl(p[0], F_SETFD, FD_CLOEXEC) == 0 &&
      fcntl(p[1], F_SETFD, FD_CLOEXEC) == 0) return 1;
  close(p[0]); close(p[1]); return 0;
}
int process_manager_start(const char *command, char *const argv[],
                          const char *workdir, int timeout_sec,
                          size_t max_stdout, size_t max_stderr,
                          char id[PROCESS_ID_SIZE]) {
  if ((!command && (!argv || !argv[0])) || !workdir || !id ||
      max_stdout > OUTPUT_LIMIT || max_stderr > OUTPUT_LIMIT) return 0;
  int out[2], err[2];
  if (!make_pipe(out)) return 0;
  if (!make_pipe(err)) { close(out[0]); close(out[1]); return 0; }
  pid_t pid = fork();
  if (!pid) {
    if (setpgid(0, 0) || dup2(out[1], 1) < 0 || dup2(err[1], 2) < 0) _exit(127);
    close(out[0]); close(out[1]); close(err[0]); close(err[1]);
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, 0) < 0) _exit(127);
    if (fd != 0) close(fd);
    if (chdir(workdir)) _exit(127);
    if (argv) execvp(argv[0], argv);
    else execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(out[1]); close(err[1]);
  if (pid > 0) {
    setpgid(pid, pid);
    int adopted = argv
      ? process_manager_adopt_argv(pid, "argv", argv, workdir, out[0], err[0],
          max_stdout, max_stderr, timeout_sec, id)
      : process_manager_adopt(pid, "shell", command, workdir, out[0], err[0],
          max_stdout, max_stderr, timeout_sec, id);
    if (adopted) return 1;
    kill(-pid, SIGKILL); kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
  }
  close(out[0]); close(err[0]); return 0;
}
static void signal_group(Record *r, int sig) {
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  if (r->lost) return;
  if (waitid(P_PID, (id_t)r->s.pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
    if (errno == ECHILD) r->lost = 1;
    return;
  }
  kill(-r->s.pgid, sig);
}
int process_manager_stop(const char *id) {
  Record *r = find(id);
  if (!r || !r->s.running) return 0;
  if (!r->s.stopping) {
    r->s.stopping = 1;
    signal_group(r, SIGTERM);
    r->force = process_manager_now_ms() + 500;
    r->drain = r->force + 1500;
  }
  return 1;
}
void process_manager_watch(const char *id) {
  Record *r = find(id);
  if (r) r->notify = 1;
}
int process_manager_completion(const char *owner, ProcessSnapshot *snapshot) {
  if (!owner || !owner[0] || !snapshot) return 0;
  for (size_t i = 0; i < count; i++) {
    Record *r = &records[i];
    if (r->notify && !r->s.running && !strcmp(owner, r->s.owner)) {
      *snapshot = r->s;
      r->notify = 0;
      return 1;
    }
  }
  return 0;
}
void process_manager_stop_owner(const char *owner) {
  if (!owner || !owner[0]) return;
  for (size_t i = 0; i < count; i++)
    if (!strcmp(records[i].s.owner, owner)) process_manager_stop(records[i].s.id);
}
int process_manager_close_owner(const char *owner) {
  if (!owner || !owner[0]) return 1;
  process_manager_stop_owner(owner);
  long long end = process_manager_now_ms() + 2500;
  for (;;) {
    process_manager_poll();
    int active = 0;
    for (size_t i = 0; i < count; i++)
      if (!strcmp(records[i].s.owner, owner) && records[i].s.running) active = 1;
    if (!active) return 1;
    if (process_manager_now_ms() >= end) return 0;
    /* No Lua callbacks while the adapter is tearing down its owner. */
    struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
  }
}
static void drain_stream(Record *r, int i) {
  char buf[4096];
  for (int n = 0; n < 16 && r->fd[i] >= 0; n++) {
    ssize_t got = read(r->fd[i], buf, sizeof(buf));
    if (got > 0) {
      size_t room = r->cap[i] - r->len[i];
      size_t take = (size_t)got < room ? (size_t)got : room;
      memcpy(r->text[i] + r->len[i], buf, take);
      r->len[i] += take; r->text[i][r->len[i]] = 0;
      r->s.output_available = 1;
      if (take < (size_t)got) { r->s.truncated = 1; r->overflow[i] = 1; }
    } else if (!got) close_stream(r, i);
    else if (errno == EINTR) continue;
    else { if (errno != EAGAIN && errno != EWOULDBLOCK) {
        r->s.truncated = 1; close_stream(r, i);
      } break; }
  }
}
void process_manager_poll(void) {
  long long now = process_manager_now_ms();
  for (size_t i = 0; i < count; i++) {
    Record *r = &records[i];
    if (!r->s.running) continue;
    /* WNOWAIT reserves the leader PID until all group signals are finished. */
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int observed = waitid(P_PID, (id_t)r->s.pid, &info, WEXITED | WNOHANG | WNOWAIT);
    if (observed < 0 && errno == ECHILD) r->lost = 1;
    drain_stream(r, 0); drain_stream(r, 1);
    if (r->deadline && now >= r->deadline && !r->s.stopping) {
      r->s.timed_out = 1; process_manager_stop(r->s.id);
    }
    if (r->s.stopping && !r->killed && now >= r->force) {
      signal_group(r, SIGKILL); r->killed = 1;
    }
    if (r->s.stopping && now >= r->drain) {
      if (r->fd[0] >= 0 || r->fd[1] >= 0) r->s.truncated = 1;
      close_stream(r, 0); close_stream(r, 1);
    }
    if (r->lost || (info.si_pid == r->s.pid && r->fd[0] < 0 && r->fd[1] < 0 &&
                    (!r->s.stopping || r->killed))) {
      /* Closed output means no normal foreground/background output remains.
       * Clean detached-output descendants before releasing the reserved PID. */
      signal_group(r, SIGKILL);
      int status;
      pid_t waited = r->lost ? -1 : waitpid(r->s.pid, &status, WNOHANG);
      if (waited == 0 || (waited < 0 && errno == EINTR)) continue;
      if (waited == r->s.pid) {
        if (WIFEXITED(status)) r->s.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) r->s.exit_code = 128 + WTERMSIG(status);
      }
      close_stream(r, 0); close_stream(r, 1);
      r->s.running = 0; r->s.finished_ms = now;
    }
  }
}
size_t process_manager_count(void) { return count; }
int process_manager_at(size_t index, ProcessSnapshot *snapshot) {
  if (index >= count || !snapshot) return 0;
  *snapshot = records[index].s; return 1;
}
int process_manager_get(const char *id, ProcessSnapshot *snapshot) {
  Record *r = find(id);
  if (!r || !snapshot) return 0;
  *snapshot = r->s; return 1;
}
char *process_manager_raw_output(const char *id, int stderr_stream) {
  Record *r = find(id);
  return r ? copy_text(r->text[!!stderr_stream]) : NULL;
}
/* One sanitizer for metadata and public output. Always process the entire
 * retained prefix before imposing display bounds or selecting complete lines. */
static char *sanitize(const char *text, size_t len) {
  char *clean = malloc(len + 1);
  if (!clean) return NULL;
  size_t used = 0;
  int state = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (state == 1) { state = c == '[' ? 2 : (c == ']' || c == 'P' || c == '^' || c == '_') ? 3 : 0; continue; }
    if (state == 2) { if (c >= 0x40 && c <= 0x7e) state = 0; continue; }
    if (state == 3) { if (c == 7) state = 0; else if (c == 27) state = 4; continue; }
    if (state == 4) { state = c == '\\' ? 0 : 3; continue; }
    if (c == 27) { state = 1; continue; }
    if (c < 32 && c != '\n' && c != '\t') continue;
    if (c == 127) continue;
    clean[used++] = (char)c;
  }
  clean[used] = 0;
  char *safe = redact_secrets_alloc(clean);
  free(clean); return safe;
}
char *process_manager_output(const char *id, int stderr_stream) {
  Record *r = find(id);
  if (!r) return NULL;
  int stream = !!stderr_stream;
  char *safe = sanitize(r->text[stream], r->len[stream]);
  if (!safe) return NULL;
  size_t n = strlen(safe);
  if (r->s.running && r->fd[stream] >= 0) {
    char *newline = strrchr(safe, '\n');
    n = newline ? (size_t)(newline - safe) + 1 : 0;
  }
  /* Redaction may expand a tiny retained prefix; apply the public byte bound
   * only now, never by slicing raw text before redaction. */
  if (n > r->cap[stream]) n = r->cap[stream];
  safe[n] = 0;
  if (r->overflow[stream]) {
    const char marker[] = "\n[output truncated]";
    char *result = realloc(safe, n + sizeof(marker));
    if (!result) { free(safe); return NULL; }
    memcpy(result + n, marker, sizeof(marker));
    safe = result;
  }
  return safe;
}
void process_manager_shutdown(void) {
  for (size_t i = 0; i < count; i++) {
    Record *r = &records[i];
    if (r->s.running) process_manager_stop(r->s.id);
  }
  long long end = process_manager_now_ms() + 2500;
  for (;;) {
    process_manager_poll();
    int active = 0;
    for (size_t i = 0; i < count; i++) active |= records[i].s.running;
    if (!active || process_manager_now_ms() >= end) break;
    struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
  }
  /* Keep any uninterruptible children registered rather than losing wait ownership. */
  for (size_t i = 0; i < count;) {
    if (records[i].s.running) { i++; continue; }
    dispose(&records[i]);
    memmove(records + i, records + i + 1, (--count - i) * sizeof(Record));
  }
}
