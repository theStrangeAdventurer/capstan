#ifndef SHELL_OUTPUT_H
#define SHELL_OUTPUT_H

#include <stddef.h>

/* Legacy threshold retained for source compatibility; folding now applies
   to every nonempty tagged body, independent of line count or wrapping. */
#define SHELL_OUTPUT_FOLD_LINES 20

typedef struct {
  size_t start, end;                 /* Original UI text range. */
  size_t body_start, body_end, lines;
  size_t join_start, join_end;       /* Newlines before status projected as a space. */
  int expanded;
  size_t view_start, view_end, control_start, control_end;
} ShellOutputBlock;

typedef struct {
  ShellOutputBlock *blocks;
  size_t count, capacity;
  char *view;                       /* Display only; never persisted. */
} ShellOutput;

int shell_output_add(ShellOutput *output, const char *text, size_t start,
                     size_t end);
const char *shell_output_build(ShellOutput *output, const char *text);
size_t shell_output_offset(const ShellOutput *output, size_t original);
int shell_output_contains(const ShellOutput *output, size_t start, size_t end);
/* A control is clickable only over its three-character [+]/[-] marker. */
int shell_output_control(const ShellOutput *output, size_t offset);
void shell_output_free(ShellOutput *output);

#endif
