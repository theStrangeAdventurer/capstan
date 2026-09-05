#include "shell_output.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int shell_output_add(ShellOutput *output, const char *text, size_t start,
                     size_t end) {
  if (!output || !text || start >= end || end > strlen(text) ||
      (output->count && start < output->blocks[output->count - 1].end))
    return 0;
  if (output->count == output->capacity) {
    size_t capacity = output->capacity ? output->capacity * 2 : 4;
    ShellOutputBlock *blocks = realloc(output->blocks, capacity * sizeof(*blocks));
    if (!blocks) return 0;
    output->blocks = blocks;
    output->capacity = capacity;
  }
  ShellOutputBlock block = {.start = start, .end = end, .body_start = start,
                             .body_end = end};
  /* Interpret the status only inside an explicitly tagged shell result.
     Model results also include a completion suffix before [exit N]. */
  for (size_t p = start; p < end;) {
    const char *newline = memchr(text + p, '\n', end - p);
    size_t next = newline ? (size_t)(newline - text) + 1 : end;
    if (end - p >= 6 && memcmp(text + p, "[exit ", 6) == 0) {
      block.body_start = next;
      size_t previous_end = output->count ? output->blocks[output->count - 1].end : 0;
      size_t join = p;
      while (join > previous_end && text[join - 1] == '\n') join--;
      /* Keep standalone results standalone; only join an existing header. */
      if (join < p && join > previous_end) {
        block.join_start = join;
        block.join_end = p;
        if (join < block.start) block.start = join;
      }
      break;
    }
    p = next;
  }
  while (block.body_end > block.body_start && text[block.body_end - 1] == '\n')
    block.body_end--;
  if (block.body_start < block.body_end) {
    block.lines = 1;
    for (size_t p = block.body_start; p < block.body_end; p++)
      if (text[p] == '\n') block.lines++;
  }
  output->blocks[output->count++] = block;
  return 1;
}

const char *shell_output_build(ShellOutput *output, const char *text) {
  free(output->view);
  output->view = NULL;
  if (!output->count) return text;
  size_t length = strlen(text);
  /* A control line fits in 64 bytes even with a size_t line count. */
  if (output->count > (SIZE_MAX - length - 1) / 64) return text;
  char *view = malloc(length + output->count * 64 + 1);
  if (!view) return text; /* Fail open: leave the complete output readable. */
  size_t src = 0, dst = 0;
  for (size_t i = 0; i < output->count; i++) {
    ShellOutputBlock *block = &output->blocks[i];
    if (block->start < src || block->end > length) {
      free(view);
      return text;
    }
    memcpy(view + dst, text + src, block->start - src);
    dst += block->start - src;
    block->view_start = dst;
    block->control_start = block->control_end = 0;
    size_t prefix_start = block->start;
    if (block->join_end > block->join_start) {
      size_t prefix = block->join_start - prefix_start;
      memcpy(view + dst, text + prefix_start, prefix);
      dst += prefix;
      view[dst++] = ' ';
      prefix_start = block->join_end;
    }
    size_t prefix = block->body_start - prefix_start;
    memcpy(view + dst, text + prefix_start, prefix);
    dst += prefix;
    if (block->lines > SHELL_OUTPUT_FOLD_LINES) {
      block->control_start = dst;
      dst += (size_t)sprintf(view + dst, "[%c] %zu lines",
                            block->expanded ? '-' : '+', block->lines);
      block->control_end = dst;
      if (block->expanded) {
        view[dst++] = '\n';
        size_t body = block->body_end - block->body_start;
        memcpy(view + dst, text + block->body_start, body);
        dst += body;
      }
      size_t suffix = block->end - block->body_end;
      memcpy(view + dst, text + block->body_end, suffix);
      dst += suffix;
    } else {
      memcpy(view + dst, text + block->body_start, block->end - block->body_start);
      dst += block->end - block->body_start;
    }
    block->view_end = dst;
    src = block->end;
  }
  memcpy(view + dst, text + src, length - src + 1);
  output->view = view;
  return view;
}

size_t shell_output_offset(const ShellOutput *output, size_t original) {
  if (!output->view) return original;
  size_t src = 0, dst = 0;
  for (size_t i = 0; i < output->count; i++) {
    const ShellOutputBlock *b = &output->blocks[i];
    if (original < b->start) return dst + original - src;
    if (original < b->end) {
      if (b->lines <= SHELL_OUTPUT_FOLD_LINES || original < b->body_start) {
        size_t mapped = original;
        if (b->join_end > b->join_start && original >= b->join_start) {
          if (original < b->join_end)
            mapped = b->join_start;
          else
            mapped -= b->join_end - b->join_start - 1;
        }
        return b->view_start + mapped - b->start;
      }
      if (original < b->body_end)
        return b->expanded ? b->control_end + 1 + original - b->body_start
                           : b->control_start;
      return b->view_end - (b->end - original);
    }
    src = b->end;
    dst = b->view_end;
  }
  return dst + original - src;
}

int shell_output_contains(const ShellOutput *output, size_t start, size_t end) {
  for (size_t i = 0; i < output->count; i++) {
    const ShellOutputBlock *b = &output->blocks[i];
    size_t first = output->view ? b->view_start : b->start;
    size_t last = output->view ? b->view_end : b->end;
    if (start < last && end > first) return 1;
  }
  return 0;
}

int shell_output_control(const ShellOutput *output, size_t offset) {
  if (!output->view) return -1;
  for (size_t i = 0; i < output->count; i++) {
    const ShellOutputBlock *b = &output->blocks[i];
    if (b->control_end > b->control_start && offset >= b->control_start &&
        offset < b->control_start + 3)
      return (int)i;
  }
  return -1;
}

void shell_output_free(ShellOutput *output) {
  free(output->view);
  free(output->blocks);
  *output = (ShellOutput){0};
}
