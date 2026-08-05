#include "clipboard.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

char *clipboard_base64_encode(const unsigned char *data, size_t size) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!data && size > 0)
    return NULL;
  if (size > (((size_t)-1) - 1) / 4 * 3)
    return NULL;
  size_t output_size = ((size + 2) / 3) * 4;
  char *output = malloc(output_size + 1);
  if (!output)
    return NULL;
  size_t in = 0, out = 0;
  while (in + 3 <= size) {
    unsigned int value = ((unsigned int)data[in] << 16) |
                         ((unsigned int)data[in + 1] << 8) | data[in + 2];
    output[out++] = alphabet[(value >> 18) & 0x3f];
    output[out++] = alphabet[(value >> 12) & 0x3f];
    output[out++] = alphabet[(value >> 6) & 0x3f];
    output[out++] = alphabet[value & 0x3f];
    in += 3;
  }
  if (in < size) {
    unsigned int value = (unsigned int)data[in] << 16;
    output[out++] = alphabet[(value >> 18) & 0x3f];
    if (in + 1 < size) {
      value |= (unsigned int)data[in + 1] << 8;
      output[out++] = alphabet[(value >> 12) & 0x3f];
      output[out++] = alphabet[(value >> 6) & 0x3f];
      output[out++] = '=';
    } else {
      output[out++] = alphabet[(value >> 12) & 0x3f];
      output[out++] = '=';
      output[out++] = '=';
    }
  }
  output[out] = '\0';
  return output;
}

ClipboardReadStatus clipboard_read_fd_limited(int fd, size_t limit,
                                               unsigned char **data,
                                               size_t *size) {
  unsigned char *buffer = NULL;
  size_t len = 0, cap = 0;
  unsigned char chunk[8192];
  if (!data || !size)
    return CLIPBOARD_READ_ERROR;
  *data = NULL;
  *size = 0;

  while (1) {
    size_t wanted = sizeof(chunk);
    if (len < limit && wanted > limit - len)
      wanted = limit - len;
    if (len == limit)
      wanted = 1;
    ssize_t n = read(fd, chunk, wanted);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      free(buffer);
      return CLIPBOARD_READ_ERROR;
    }
    if (n == 0)
      break;
    if (len == limit) {
      free(buffer);
      return CLIPBOARD_READ_OVERFLOW;
    }
    size_t needed = len + (size_t)n;
    if (needed > cap) {
      size_t next = cap ? cap * 2 : sizeof(chunk);
      if (next > limit)
        next = limit;
      while (next < needed) {
        size_t doubled = next > limit / 2 ? limit : next * 2;
        if (doubled == next) {
          free(buffer);
          return CLIPBOARD_READ_ERROR;
        }
        next = doubled;
      }
      unsigned char *grown = realloc(buffer, next);
      if (!grown) {
        free(buffer);
        return CLIPBOARD_READ_ERROR;
      }
      buffer = grown;
      cap = next;
    }
    memcpy(buffer + len, chunk, (size_t)n);
    len = needed;
  }
  if (len == 0) {
    free(buffer);
    return CLIPBOARD_READ_EMPTY;
  }
  *data = buffer;
  *size = len;
  return CLIPBOARD_READ_OK;
}

typedef enum {
  CAPTURE_OK = 0,
  CAPTURE_EMPTY,
  CAPTURE_OVERFLOW,
  CAPTURE_FAILED
} CaptureStatus;

static void reap_terminated_child(pid_t pid) {
  if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
    kill(pid, SIGKILL);
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int attempt = 0; attempt < 10; attempt++) {
    pid_t result = waitpid(pid, NULL, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD))
      return;
    if (result < 0 && errno != EINTR)
      break;
    nanosleep(&pause, NULL);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
}

static CaptureStatus capture(char *const argv[], unsigned char **data,
                             size_t *size) {
  int pipefd[2];
  *data = NULL;
  *size = 0;
  if (pipe(pipefd) != 0)
    return CAPTURE_FAILED;
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return CAPTURE_FAILED;
  }
  if (pid == 0) {
    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0)
      _exit(127);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pipefd[1]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(pipefd[1]);
  ClipboardReadStatus read_status = clipboard_read_fd_limited(
      pipefd[0], CLIPBOARD_IMAGE_MAX_BYTES, data, size);
  close(pipefd[0]);
  if (read_status == CLIPBOARD_READ_OVERFLOW) {
    reap_terminated_child(pid);
    return CAPTURE_OVERFLOW;
  }

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      read_status == CLIPBOARD_READ_ERROR) {
    free(*data);
    *data = NULL;
    *size = 0;
    return CAPTURE_FAILED;
  }
  return read_status == CLIPBOARD_READ_EMPTY ? CAPTURE_EMPTY : CAPTURE_OK;
}

#ifdef __APPLE__
static CaptureStatus read_macos(unsigned char **data, size_t *size) {
  char path[] = "/tmp/capstan-clipboard-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return CAPTURE_FAILED;
  close(fd);
  char *const argv[] = {
      "osascript", "-e", "on run argv", "-e",
      "set imageData to the clipboard as \"PNGf\"", "-e",
      "set fileRef to open for access POSIX file (item 1 of argv) with write permission",
      "-e", "set eof fileRef to 0", "-e", "write imageData to fileRef",
      "-e", "close access fileRef", "-e", "end run", path, NULL};
  size_t output_size = 0;
  unsigned char *output = NULL;
  CaptureStatus command_status = capture(argv, &output, &output_size);
  free(output);
  if (command_status == CAPTURE_FAILED || command_status == CAPTURE_OVERFLOW) {
    unlink(path);
    return command_status;
  }
  fd = open(path, O_RDONLY);
  unlink(path);
  if (fd < 0)
    return CAPTURE_FAILED;
  ClipboardReadStatus read_status = clipboard_read_fd_limited(
      fd, CLIPBOARD_IMAGE_MAX_BYTES, data, size);
  close(fd);
  if (read_status == CLIPBOARD_READ_OK)
    return CAPTURE_OK;
  if (read_status == CLIPBOARD_READ_OVERFLOW)
    return CAPTURE_OVERFLOW;
  return read_status == CLIPBOARD_READ_EMPTY ? CAPTURE_EMPTY : CAPTURE_FAILED;
}
#endif

static int is_png(const unsigned char *data, size_t size) {
  static const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                             0x1a, '\n'};
  return data && size >= sizeof(signature) &&
         memcmp(data, signature, sizeof(signature)) == 0;
}

static unsigned char *validated_png(unsigned char *data, size_t *size,
                                    CaptureStatus *status) {
  if (*status == CAPTURE_OK && is_png(data, *size))
    return data;
  free(data);
  if (*status == CAPTURE_OK)
    *status = CAPTURE_FAILED;
  *size = 0;
  return NULL;
}

static void set_clipboard_error(char *error, size_t error_size,
                                CaptureStatus status) {
  if (!error || error_size == 0)
    return;
  if (status == CAPTURE_OVERFLOW) {
    snprintf(error, error_size,
             "Clipboard image exceeds the 10 MiB size limit");
  } else if (status == CAPTURE_EMPTY) {
    snprintf(error, error_size, "Clipboard does not contain a PNG image");
  } else {
    snprintf(error, error_size,
             "Could not read clipboard image; install or enable a supported clipboard reader");
  }
}

unsigned char *clipboard_read_image(size_t *size, char *error,
                                    size_t error_size) {
  size_t local_size = 0;
  unsigned char *data = NULL;
  CaptureStatus status = CAPTURE_FAILED;
  size_t *result_size = size ? size : &local_size;
  *result_size = 0;
  if (error && error_size)
    error[0] = '\0';
#ifdef __APPLE__
  status = read_macos(&data, result_size);
  if (status == CAPTURE_OK) {
    unsigned char *validated = validated_png(data, result_size, &status);
    if (validated)
      return validated;
  }
#elif defined(__linux__)
  if (getenv("WAYLAND_DISPLAY")) {
    char *const wayland[] = {"wl-paste", "-t", "image/png", NULL};
    status = capture(wayland, &data, result_size);
    if (status == CAPTURE_OK) {
      unsigned char *validated = validated_png(data, result_size, &status);
      if (validated)
        return validated;
    }
    if (status == CAPTURE_OVERFLOW) {
      set_clipboard_error(error, error_size, status);
      return NULL;
    }
  }
  char *const x11[] = {"xclip", "-selection", "clipboard", "-t",
                       "image/png", "-o", NULL};
  CaptureStatus x11_status = capture(x11, &data, result_size);
  if (x11_status == CAPTURE_OK) {
    unsigned char *validated = validated_png(data, result_size, &x11_status);
    if (validated)
      return validated;
  }
  if (x11_status == CAPTURE_OVERFLOW || status != CAPTURE_EMPTY)
    status = x11_status;
#endif
  set_clipboard_error(error, error_size, status);
  return NULL;
}
