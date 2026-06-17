#ifndef USAGE_H
#define USAGE_H

#include <stddef.h>

typedef struct {
  int prompt_tokens;
  int completion_tokens;
  int total_tokens;
  int context_limit;
} UsageStats;

int usage_has_values(UsageStats usage);
int usage_format(UsageStats usage, char *buf, size_t buf_size);

#endif
