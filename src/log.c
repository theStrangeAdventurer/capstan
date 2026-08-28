#include "log.h"
#include "app_config.h"
#include "jsonl.h"
#include "redact.h"
#include "session.h"
#include <lauxlib.h>
#include <lua.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_MAX_BYTES (10L * 1024L * 1024L)
#define LOG_MAX_ARCHIVES 5
#define LOG_MAX_READ_BYTES (1024L * 1024L)

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

static lua_State *g_log_lua = NULL;
static char g_log_session_id[SESSION_ID_SIZE] = "";
static int g_log_read_lock_fd = -1;
static char g_log_read_lock_path[512] = "";

int log_path(char *buf, size_t buf_size) {
  time_t now = time(NULL);
  struct tm tm_buf;
  struct tm *tm = localtime_r(&now, &tm_buf);
  char name[64];
  if (tm) {
    if (strftime(name, sizeof(name), "%Y-%m-%d.jsonl", tm) == 0)
      return -1;
  } else {
    snprintf(name, sizeof(name), "unknown-date.jsonl");
  }
  char relative[512];
  int count;
  if (g_log_session_id[0])
    count = snprintf(relative, sizeof(relative), "logs/sessions/%s/%s",
                     g_log_session_id, name);
  else
    count = snprintf(relative, sizeof(relative), "logs/%s", name);
  if (count < 0 || (size_t)count >= sizeof(relative))
    return -1;
  return app_state_path(buf, buf_size, relative);
}

int log_set_session_id(const char *session_id) {
  if (!session_id || !session_id[0]) {
    g_log_session_id[0] = '\0';
    return 1;
  }
  if (!session_id_valid(session_id))
    return 0;
  snprintf(g_log_session_id, sizeof(g_log_session_id), "%s", session_id);
  return 1;
}

const char *log_session_id(void) { return g_log_session_id; }

static int ensure_dir(const char *path, mode_t mode) {
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (!S_ISDIR(st.st_mode))
      return -1;
    return chmod(path, mode) == 0 ? 0 : -1;
  }
  if (errno != ENOENT)
    return -1;
  if (mkdir(path, mode) == 0)
    return chmod(path, mode) == 0 ? 0 : -1;
  if (errno == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
    return chmod(path, mode) == 0 ? 0 : -1;
  return -1;
}

static int ensure_log_dir(void) {
  char logs[512];
  if (app_state_path(logs, sizeof(logs), "logs") != 0 ||
      ensure_dir(logs, 0700) != 0)
    return -1;

  if (!g_log_session_id[0])
    return 0;

  char sessions[512];
  int count = snprintf(sessions, sizeof(sessions), "%s/sessions", logs);
  if (count < 0 || (size_t)count >= sizeof(sessions) ||
      ensure_dir(sessions, 0700) != 0)
    return -1;
  char scoped[512];
  count = snprintf(scoped, sizeof(scoped), "%s/%s", sessions,
                   g_log_session_id);
  if (count < 0 || (size_t)count >= sizeof(scoped))
    return -1;
  return ensure_dir(scoped, 0700);
}

static char *dup_string(const char *text) {
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (!copy)
    return NULL;
  memcpy(copy, text, length + 1);
  return copy;
}

static char *lua_redact_alloc(const char *message) {
  if (!g_log_lua)
    return NULL;

  lua_State *state = g_log_lua;
  int top = lua_gettop(state);
  lua_getglobal(state, "require");
  if (!lua_isfunction(state, -1)) {
    lua_settop(state, top);
    return NULL;
  }
  lua_pushstring(state, "agent.redact");
  if (lua_pcall(state, 1, 1, 0) != LUA_OK || !lua_istable(state, -1)) {
    lua_settop(state, top);
    return NULL;
  }
  lua_getfield(state, -1, "text");
  if (!lua_isfunction(state, -1)) {
    lua_settop(state, top);
    return NULL;
  }
  lua_pushstring(state, message ? message : "");
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    lua_settop(state, top);
    return NULL;
  }
  const char *redacted = lua_tostring(state, -1);
  char *copy = dup_string(redacted ? redacted : "");
  lua_settop(state, top);
  return copy;
}

static int rotated_path(const char *path, int archive_index, char *buffer,
                        size_t buffer_size) {
  const char *suffix = ".jsonl";
  size_t path_length = strlen(path);
  size_t suffix_length = strlen(suffix);
  if (path_length < suffix_length ||
      strcmp(path + path_length - suffix_length, suffix) != 0)
    return -1;
  int count = snprintf(buffer, buffer_size, "%.*s.%d.jsonl",
                       (int)(path_length - suffix_length), path,
                       archive_index);
  return count < 0 || (size_t)count >= buffer_size ? -1 : 0;
}

static int regular_or_missing(const char *path, int *exists) {
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (!S_ISREG(st.st_mode))
      return 0;
    if (exists)
      *exists = 1;
    return 1;
  }
  if (errno != ENOENT)
    return 0;
  if (exists)
    *exists = 0;
  return 1;
}

static int secure_open_regular(const char *path, int flags, mode_t mode);
static int secure_open_read_regular(const char *path);

static int rotation_path(const char *path, char *buffer, size_t buffer_size) {
  int count = snprintf(buffer, buffer_size, "%s.rotate", path);
  return count < 0 || (size_t)count >= buffer_size ? -1 : 0;
}

static int rotation_file_path(const char *transaction, const char *name,
                              char *buffer, size_t buffer_size) {
  int count = snprintf(buffer, buffer_size, "%s/%s", transaction, name);
  return count < 0 || (size_t)count >= buffer_size ? -1 : 0;
}

static int remove_if_present(const char *path) {
  return unlink(path) == 0 || errno == ENOENT;
}

static int cleanup_rotation(const char *transaction) {
  char staged[640];
  for (int index = 1; index <= LOG_MAX_ARCHIVES; index++) {
    char name[32];
    snprintf(name, sizeof(name), "%d.jsonl", index);
    if (rotation_file_path(transaction, name, staged, sizeof(staged)) != 0 ||
        !remove_if_present(staged))
      return 0;
  }
  if (rotation_file_path(transaction, "empty", staged, sizeof(staged)) != 0 ||
      !remove_if_present(staged) ||
      rotation_file_path(transaction, "ready", staged, sizeof(staged)) != 0 ||
      !remove_if_present(staged))
    return 0;
  return rmdir(transaction) == 0 || errno == ENOENT;
}

static int copy_fd(int source_fd, int destination_fd) {
  char buffer[16384];
  for (;;) {
    ssize_t count = read(source_fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return 0;
    if (count == 0)
      return 1;
    size_t offset = 0;
    while (offset < (size_t)count) {
      ssize_t written =
          write(destination_fd, buffer + offset, (size_t)count - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0) {
        if (written == 0)
          errno = EIO;
        return 0;
      }
      offset += (size_t)written;
    }
  }
}

static int snapshot_file(const char *source, const char *destination) {
  int destination_fd = secure_open_regular(
      destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (destination_fd < 0)
    return 0;

  int exists = 0;
  int ok = source ? regular_or_missing(source, &exists) : 1;
  if (ok && source && exists) {
    int source_fd = secure_open_read_regular(source);
    if (source_fd < 0) {
      ok = 0;
    } else {
      ok = copy_fd(source_fd, destination_fd);
      if (close(source_fd) != 0)
        ok = 0;
    }
  }
  if (ok && fsync(destination_fd) != 0)
    ok = 0;
  if (close(destination_fd) != 0)
    ok = 0;
  return ok;
}

typedef struct {
  unsigned long long device;
  unsigned long long inode;
} RotationIdentity;

static int rotation_identity(const char *path, RotationIdentity *identity) {
  struct stat st;
  if (!identity || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_nlink != 1)
    return 0;
  identity->device = (unsigned long long)st.st_dev;
  identity->inode = (unsigned long long)st.st_ino;
  return 1;
}

static int rotation_identity_equal(const RotationIdentity *left,
                                   const RotationIdentity *right) {
  return left && right && left->device == right->device &&
         left->inode == right->inode;
}

static int write_bytes(int fd, const char *data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    ssize_t written = write(fd, data + offset, length - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      if (written == 0)
        errno = EIO;
      return 0;
    }
    offset += (size_t)written;
  }
  return 1;
}

static int write_rotation_manifest(const char *transaction,
                                   const char *ready) {
  char manifest[1024];
  size_t used = 0;
  for (int index = 1; index <= LOG_MAX_ARCHIVES; index++) {
    char name[32];
    char staged[640];
    RotationIdentity identity;
    snprintf(name, sizeof(name), "%d.jsonl", index);
    if (rotation_file_path(transaction, name, staged, sizeof(staged)) != 0 ||
        !rotation_identity(staged, &identity))
      return 0;
    int count = snprintf(manifest + used, sizeof(manifest) - used,
                         "%d %llu %llu\n", index, identity.device,
                         identity.inode);
    if (count < 0 || (size_t)count >= sizeof(manifest) - used)
      return 0;
    used += (size_t)count;
  }

  char empty[640];
  RotationIdentity empty_identity;
  if (rotation_file_path(transaction, "empty", empty, sizeof(empty)) != 0 ||
      !rotation_identity(empty, &empty_identity))
    return 0;
  int count = snprintf(manifest + used, sizeof(manifest) - used,
                       "0 %llu %llu\n", empty_identity.device,
                       empty_identity.inode);
  if (count < 0 || (size_t)count >= sizeof(manifest) - used)
    return 0;
  used += (size_t)count;

  int fd = secure_open_regular(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 0;
  int ok = write_bytes(fd, manifest, used) && fsync(fd) == 0;
  if (close(fd) != 0)
    ok = 0;
  return ok;
}

static int read_rotation_manifest(const char *ready,
                                  RotationIdentity identities[],
                                  RotationIdentity *empty_identity) {
  int fd = secure_open_read_regular(ready);
  if (fd < 0)
    return 0;
  char manifest[1024];
  size_t used = 0;
  int ok = 1;
  for (;;) {
    if (used == sizeof(manifest) - 1) {
      ok = 0;
      break;
    }
    ssize_t count = read(fd, manifest + used, sizeof(manifest) - 1 - used);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      ok = 0;
      break;
    }
    if (count == 0)
      break;
    used += (size_t)count;
  }
  if (close(fd) != 0)
    ok = 0;
  manifest[used] = '\0';
  if (!ok)
    return 0;

  char *cursor = manifest;
  for (int position = 1; position <= LOG_MAX_ARCHIVES + 1; position++) {
    char *newline = strchr(cursor, '\n');
    if (!newline)
      return 0;
    *newline = '\0';
    int parsed_index = -1;
    unsigned long long device = 0;
    unsigned long long inode = 0;
    char extra = '\0';
    if (sscanf(cursor, "%d %llu %llu %c", &parsed_index, &device, &inode,
               &extra) != 3)
      return 0;
    int expected = position <= LOG_MAX_ARCHIVES ? position : 0;
    if (parsed_index != expected || device == 0 || inode == 0)
      return 0;
    RotationIdentity value = {.device = device, .inode = inode};
    if (expected == 0)
      *empty_identity = value;
    else
      identities[expected] = value;
    cursor = newline + 1;
  }
  return *cursor == '\0';
}

static int fsync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return 0;
  struct stat st;
  int ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && fsync(fd) == 0;
  int saved = ok ? 0 : (errno ? errno : EIO);
  if (close(fd) != 0 && ok) {
    ok = 0;
    saved = errno;
  }
  if (!ok)
    errno = saved;
  return ok;
}

static int fsync_parent_directory(const char *path) {
  char parent[640];
  if (!path || snprintf(parent, sizeof(parent), "%s", path) >=
                   (int)sizeof(parent)) {
    errno = ENAMETOOLONG;
    return 0;
  }
  char *slash = strrchr(parent, '/');
  if (!slash) {
    snprintf(parent, sizeof(parent), ".");
  } else if (slash == parent) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return fsync_directory(parent);
}

static int recover_rotation(const char *path) {
  char transaction[640];
  if (rotation_path(path, transaction, sizeof(transaction)) != 0)
    return 0;
  struct stat transaction_st;
  if (lstat(transaction, &transaction_st) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(transaction_st.st_mode))
    return 0;

  char ready[640];
  if (rotation_file_path(transaction, "ready", ready, sizeof(ready)) != 0)
    return 0;
  struct stat ready_st;
  if (lstat(ready, &ready_st) != 0) {
    if (errno != ENOENT)
      return 0;
    return cleanup_rotation(transaction);
  }
  if (!S_ISREG(ready_st.st_mode) || ready_st.st_nlink != 1)
    return 0;
  RotationIdentity identities[LOG_MAX_ARCHIVES + 1] = {{0}};
  RotationIdentity empty_identity = {0};
  if (!read_rotation_manifest(ready, identities, &empty_identity))
    return 0;

  for (int index = LOG_MAX_ARCHIVES; index >= 1; index--) {
    char name[32];
    char staged[640];
    char archive[512];
    snprintf(name, sizeof(name), "%d.jsonl", index);
    if (rotation_file_path(transaction, name, staged, sizeof(staged)) != 0 ||
        rotated_path(path, index, archive, sizeof(archive)) != 0)
      return 0;
    struct stat staged_st;
    if (lstat(staged, &staged_st) == 0) {
      RotationIdentity staged_identity = {
          .device = (unsigned long long)staged_st.st_dev,
          .inode = (unsigned long long)staged_st.st_ino,
      };
      if (!S_ISREG(staged_st.st_mode) || staged_st.st_nlink != 1 ||
          !rotation_identity_equal(&staged_identity, &identities[index]) ||
          rename(staged, archive) != 0)
        return 0;
    } else if (errno == ENOENT) {
      RotationIdentity published;
      if (!rotation_identity(archive, &published) ||
          !rotation_identity_equal(&published, &identities[index]))
        return 0;
    } else {
      return 0;
    }
  }

  char empty[640];
  if (rotation_file_path(transaction, "empty", empty, sizeof(empty)) != 0)
    return 0;
  struct stat empty_st;
  if (lstat(empty, &empty_st) == 0) {
    RotationIdentity staged_empty = {
        .device = (unsigned long long)empty_st.st_dev,
        .inode = (unsigned long long)empty_st.st_ino,
    };
    if (!S_ISREG(empty_st.st_mode) || empty_st.st_nlink != 1 ||
        !rotation_identity_equal(&staged_empty, &empty_identity) ||
        rename(empty, path) != 0)
      return 0;
  } else if (errno == ENOENT) {
    RotationIdentity published_empty;
    if (!rotation_identity(path, &published_empty) ||
        !rotation_identity_equal(&published_empty, &empty_identity))
      return 0;
  } else {
    return 0;
  }

  /* Persist removal of every staged entry and publication of every archive
   * before removing the recovery manifest. rename() changes both directories,
   * so syncing only the destination would leave crash recovery ambiguous. */
  if (!fsync_directory(transaction) || !fsync_parent_directory(path) ||
      !remove_if_present(ready) || !fsync_directory(transaction))
    return 0;
  if (!cleanup_rotation(transaction))
    return 0;
  return fsync_parent_directory(path);
}

static int build_rotation(const char *path) {
  char transaction[640];
  if (rotation_path(path, transaction, sizeof(transaction)) != 0 ||
      mkdir(transaction, 0700) != 0)
    return 0;
  if (chmod(transaction, 0700) != 0) {
    (void)cleanup_rotation(transaction);
    return 0;
  }

  int ok = 1;
  for (int index = 1; index <= LOG_MAX_ARCHIVES && ok; index++) {
    char name[32];
    char staged[640];
    char source[512];
    snprintf(name, sizeof(name), "%d.jsonl", index);
    if (rotation_file_path(transaction, name, staged, sizeof(staged)) != 0) {
      ok = 0;
      break;
    }
    if (index == 1) {
      snprintf(source, sizeof(source), "%s", path);
    } else if (rotated_path(path, index - 1, source, sizeof(source)) != 0) {
      ok = 0;
      break;
    }
    ok = snapshot_file(source, staged);
  }

  char empty[640];
  char ready[640];
  if (ok &&
      (rotation_file_path(transaction, "empty", empty, sizeof(empty)) != 0 ||
       !snapshot_file(NULL, empty)))
    ok = 0;
  if (ok &&
      (rotation_file_path(transaction, "ready", ready, sizeof(ready)) != 0 ||
       !write_rotation_manifest(transaction, ready)))
    ok = 0;

  int transaction_fd = open(transaction, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                             O_NOFOLLOW);
  if (ok && (transaction_fd < 0 || fsync(transaction_fd) != 0))
    ok = 0;
  if (transaction_fd >= 0 && close(transaction_fd) != 0)
    ok = 0;
  if (!ok) {
    (void)cleanup_rotation(transaction);
    return 0;
  }
  return recover_rotation(path);
}

static int rotate_if_needed(const char *path) {
  if (!recover_rotation(path))
    return 0;

  struct stat current;
  if (lstat(path, &current) != 0)
    return errno == ENOENT;
  if (!S_ISREG(current.st_mode))
    return 0;
  if (current.st_size < LOG_MAX_BYTES)
    return 1;

  char archive[512];
  for (int index = 1; index <= LOG_MAX_ARCHIVES; index++) {
    if (rotated_path(path, index, archive, sizeof(archive)) != 0 ||
        !regular_or_missing(archive, NULL))
      return 0;
  }
  return build_rotation(path);
}

static int secure_open_regular(const char *path, int flags, mode_t mode) {
  struct stat before;
  int existed = lstat(path, &before) == 0;
  if (existed && !S_ISREG(before.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  if (!existed && errno != ENOENT)
    return -1;

  int fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0)
    return -1;
  struct stat opened;
  struct stat current;
  if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
      opened.st_nlink != 1 || lstat(path, &current) != 0 ||
      !S_ISREG(current.st_mode) || current.st_dev != opened.st_dev ||
      current.st_ino != opened.st_ino || fchmod(fd, mode) != 0) {
    int saved = errno ? errno : EINVAL;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static int secure_open_read_regular(const char *path) {
  struct stat before;
  if (lstat(path, &before) != 0)
    return -1;
  if (!S_ISREG(before.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;
  struct stat opened;
  struct stat current;
  if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
      opened.st_nlink != 1 || lstat(path, &current) != 0 ||
      !S_ISREG(current.st_mode) || current.st_dev != opened.st_dev ||
      current.st_ino != opened.st_ino) {
    int saved = errno ? errno : EINVAL;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static int log_candidate_allowed(const char *requested,
                                 const char *locked_current) {
  if (!requested || !locked_current || !locked_current[0])
    return 0;
  const char *current = locked_current;
  if (strcmp(requested, current) == 0)
    return 1;

  char candidate[512];
  for (int index = 1; index <= LOG_MAX_ARCHIVES; index++) {
    if (rotated_path(current, index, candidate, sizeof(candidate)) == 0 &&
        strcmp(requested, candidate) == 0)
      return 1;
  }

  const char *suffix = ".jsonl";
  size_t current_len = strlen(current);
  size_t suffix_len = strlen(suffix);
  if (current_len < suffix_len ||
      strcmp(current + current_len - suffix_len, suffix) != 0)
    return 0;
  int count = snprintf(candidate, sizeof(candidate), "%.*s.log",
                       (int)(current_len - suffix_len), current);
  if (count >= 0 && (size_t)count < sizeof(candidate) &&
      strcmp(requested, candidate) == 0)
    return 1;
  for (int index = 1; index <= LOG_MAX_ARCHIVES; index++) {
    count = snprintf(candidate, sizeof(candidate), "%.*s.%d.log",
                     (int)(current_len - suffix_len), current, index);
    if (count >= 0 && (size_t)count < sizeof(candidate) &&
        strcmp(requested, candidate) == 0)
      return 1;
  }
  return 0;
}

static int lock_log(const char *path) {
  char lock_path[520];
  if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >=
      (int)sizeof(lock_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  int fd = secure_open_regular(lock_path, O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    return -1;
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  while (fcntl(fd, F_SETLKW, &lock) != 0) {
    if (errno == EINTR)
      continue;
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }

  struct stat locked;
  struct stat current;
  if (fstat(fd, &locked) != 0 || !S_ISREG(locked.st_mode) ||
      locked.st_nlink != 1 || lstat(lock_path, &current) != 0 ||
      !S_ISREG(current.st_mode) || current.st_dev != locked.st_dev ||
      current.st_ino != locked.st_ino) {
    int saved = errno ? errno : ESTALE;
    lock.l_type = F_UNLCK;
    (void)fcntl(fd, F_SETLK, &lock);
    (void)close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static void unlock_log(int fd) {
  if (fd < 0)
    return;
  struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
  (void)fcntl(fd, F_SETLK, &lock);
  (void)close(fd);
}

int log_event_level(const char *level, const char *category,
                    const char *message) {
  char path[512];
  if (app_state_ensure_dir() != 0 || ensure_log_dir() != 0 ||
      log_path(path, sizeof(path)) != 0)
    return 0;

  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    return 0;
  struct tm tm_buf;
  struct tm *tm = gmtime_r(&now.tv_sec, &tm_buf);
  char timestamp[40];
  if (tm) {
    size_t length = strftime(timestamp, sizeof(timestamp),
                             "%Y-%m-%dT%H:%M:%S", tm);
    if (length == 0 ||
        snprintf(timestamp + length, sizeof(timestamp) - length, ".%03ldZ",
                 now.tv_nsec / 1000000L) >=
            (int)(sizeof(timestamp) - length))
      return 0;
  } else {
    snprintf(timestamp, sizeof(timestamp), "unknown-time");
  }

  char *redacted = lua_redact_alloc(message ? message : "");
  if (!redacted)
    redacted = redact_secrets_alloc(message ? message : "");
  if (!redacted)
    redacted = dup_string("[REDACTION_FAILED]");
  if (!redacted)
    return 0;

  JsonlBuffer line;
  jsonl_buffer_init(&line);
  int built = jsonl_append(&line,
                           "{\"schema\":\"capstan.log.v1\",\"timestamp\":") &&
              jsonl_append_string(&line, timestamp) &&
              jsonl_append(&line, ",\"level\":") &&
              jsonl_append_string(&line, level ? level : "info") &&
              jsonl_append(&line, ",\"category\":") &&
              jsonl_append_string(&line, category ? category : "event");
  if (built && g_log_session_id[0])
    built = jsonl_append(&line, ",\"session_id\":") &&
            jsonl_append_string(&line, g_log_session_id);
  built = built && jsonl_append(&line, ",\"message\":") &&
          jsonl_append_string(&line, redacted) && jsonl_append(&line, "}");
  free(redacted);
  if (!built) {
    jsonl_buffer_free(&line);
    return 0;
  }

  int result = 0;
  int lock_fd = lock_log(path);
  if (lock_fd >= 0) {
    if (rotate_if_needed(path)) {
      int fd = secure_open_regular(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
      if (fd >= 0) {
        result = jsonl_write_line(fd, &line);
        if (close(fd) != 0)
          result = 0;
      }
    }
    unlock_log(lock_fd);
  }
  jsonl_buffer_free(&line);
  return result;
}

int log_event(const char *category, const char *message) {
  return log_event_level("info", category, message);
}

static int l_capstan_log(lua_State *state) {
  const char *category = luaL_checkstring(state, 1);
  const char *message = luaL_optstring(state, 2, "");
  const char *level = luaL_optstring(state, 3, "info");
  lua_pushboolean(state, log_event_level(level, category, message));
  return 1;
}

static int l_capstan_log_path(lua_State *state) {
  char path[512];
  if (log_path(path, sizeof(path)) != 0) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushstring(state, path);
  return 1;
}

static int l_capstan_log_read_lock(lua_State *state) {
  char path[512];
  if (g_log_read_lock_fd >= 0 || app_state_ensure_dir() != 0 ||
      ensure_log_dir() != 0 || log_path(path, sizeof(path)) != 0) {
    lua_pushboolean(state, 0);
    return 1;
  }
  g_log_read_lock_fd = lock_log(path);
  if (g_log_read_lock_fd >= 0 && !recover_rotation(path)) {
    unlock_log(g_log_read_lock_fd);
    g_log_read_lock_fd = -1;
  }
  if (g_log_read_lock_fd >= 0)
    snprintf(g_log_read_lock_path, sizeof(g_log_read_lock_path), "%s", path);
  else
    g_log_read_lock_path[0] = '\0';
  lua_pushboolean(state, g_log_read_lock_fd >= 0);
  if (g_log_read_lock_fd >= 0)
    lua_pushstring(state, g_log_read_lock_path);
  else
    lua_pushnil(state);
  return 2;
}

static int l_capstan_log_read_unlock(lua_State *state) {
  (void)state;
  unlock_log(g_log_read_lock_fd);
  g_log_read_lock_fd = -1;
  g_log_read_lock_path[0] = '\0';
  return 0;
}

static int l_capstan_log_read_tail(lua_State *state) {
  const char *path = luaL_checkstring(state, 1);
  lua_Integer requested = luaL_checkinteger(state, 2);
  if (g_log_read_lock_fd < 0)
    return luaL_error(state, "runtime log read requires the log lock");
  if (!log_candidate_allowed(path, g_log_read_lock_path))
    return luaL_error(state, "runtime log path is not an allowed log file");
  if (requested < 0)
    requested = 0;
  if (requested > LOG_MAX_READ_BYTES)
    requested = LOG_MAX_READ_BYTES;

  int fd = secure_open_read_regular(path);
  if (fd < 0) {
    if (errno == ENOENT) {
      lua_pushnil(state);
      lua_pushinteger(state, 0);
      return 2;
    }
    return luaL_error(state, "cannot safely read runtime log: %s",
                      strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0) {
    int saved = errno ? errno : EIO;
    close(fd);
    return luaL_error(state, "cannot inspect runtime log: %s",
                      strerror(saved));
  }
  off_t amount = st.st_size < requested ? st.st_size : (off_t)requested;
  off_t start = st.st_size - amount;
  if (lseek(fd, start, SEEK_SET) < 0) {
    int saved = errno;
    close(fd);
    return luaL_error(state, "cannot seek runtime log: %s", strerror(saved));
  }
  char *buffer = malloc((size_t)amount + 1);
  if (!buffer) {
    close(fd);
    return luaL_error(state, "cannot allocate runtime log read buffer");
  }
  size_t offset = 0;
  while (offset < (size_t)amount) {
    ssize_t count = read(fd, buffer + offset, (size_t)amount - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      int saved = count == 0 ? EIO : errno;
      free(buffer);
      close(fd);
      return luaL_error(state, "cannot read runtime log: %s",
                        strerror(saved));
    }
    offset += (size_t)count;
  }
  close(fd);
  lua_pushlstring(state, buffer, offset);
  lua_pushinteger(state, (lua_Integer)start);
  free(buffer);
  return 2;
}

void log_init(lua_State *state) {
  g_log_lua = state;
  lua_getglobal(state, "capstan");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
  }
  lua_pushcfunction(state, l_capstan_log);
  lua_setfield(state, -2, "log");
  lua_pushcfunction(state, l_capstan_log_path);
  lua_setfield(state, -2, "log_path");
  lua_pushcfunction(state, l_capstan_log_read_lock);
  lua_setfield(state, -2, "log_read_lock");
  lua_pushcfunction(state, l_capstan_log_read_unlock);
  lua_setfield(state, -2, "log_read_unlock");
  lua_pushcfunction(state, l_capstan_log_read_tail);
  lua_setfield(state, -2, "log_read_tail");
  lua_setglobal(state, "capstan");
}

void log_cleanup(void) {
  if (g_log_read_lock_fd >= 0) {
    unlock_log(g_log_read_lock_fd);
    g_log_read_lock_fd = -1;
  }
  g_log_read_lock_path[0] = '\0';
  g_log_lua = NULL;
  g_log_session_id[0] = '\0';
}
