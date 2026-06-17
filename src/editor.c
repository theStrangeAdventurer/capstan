#include "editor.h"
#include "input.h"
#include <ncursesw/curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *shell_quote(const char *s) {
  size_t extra = 3;
  for (const char *p = s; *p; p++) {
    extra += (*p == '\'') ? 4 : 1;
  }

  char *quoted = malloc(extra);
  if (!quoted)
    return NULL;

  char *out = quoted;
  *out++ = '\'';
  for (const char *p = s; *p; p++) {
    if (*p == '\'') {
      memcpy(out, "'\\''", 4);
      out += 4;
    } else {
      *out++ = *p;
    }
  }
  *out++ = '\'';
  *out = '\0';
  return quoted;
}

static int write_initial_text(int fd, const char *text) {
  size_t len = strlen(text);
  while (len > 0) {
    ssize_t n = write(fd, text, len);
    if (n <= 0)
      return -1;
    text += n;
    len -= (size_t)n;
  }
  return 0;
}

static int read_prompt_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;

  char buf[INPUT_BUFFER_SIZE];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  int failed = ferror(f);
  fclose(f);
  if (failed)
    return -1;

  buf[n] = '\0';
  input_set_text(buf);
  return 0;
}

int editor_open_prompt(const char *initial_text) {
  const char *editor = getenv("EDITOR");
  if (!editor || !editor[0])
    editor = getenv("editor");
  if (!editor || !editor[0]) {
    input_set_text("EDITOR is not set");
    return -1;
  }

  char path[] = "/tmp/termai-editor-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    input_set_text("Failed to create editor temp file");
    return -1;
  }

  if (write_initial_text(fd, initial_text ? initial_text : "") != 0) {
    close(fd);
    unlink(path);
    input_set_text("Failed to write editor temp file");
    return -1;
  }
  close(fd);

  char *quoted_path = shell_quote(path);
  if (!quoted_path) {
    unlink(path);
    input_set_text("Failed to allocate editor command");
    return -1;
  }

  size_t command_len = strlen(editor) + strlen(quoted_path) + 2;
  char *command = malloc(command_len);
  if (!command) {
    free(quoted_path);
    unlink(path);
    input_set_text("Failed to allocate editor command");
    return -1;
  }
  snprintf(command, command_len, "%s %s", editor, quoted_path);
  free(quoted_path);

  def_prog_mode();
  endwin();
  int status = system(command);
  reset_prog_mode();
  refresh();

  free(command);

  int ok = status != -1;
  if (ok && WIFEXITED(status))
    ok = WEXITSTATUS(status) == 0;
  else
    ok = 0;

  if (!ok) {
    unlink(path);
    input_set_text("Editor exited with an error");
    return -1;
  }

  int result = read_prompt_file(path);
  unlink(path);
  if (result != 0) {
    input_set_text("Failed to read editor temp file");
    return -1;
  }

  return 0;
}
