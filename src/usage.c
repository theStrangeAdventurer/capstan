#include "usage.h"
#include <stdio.h>
#include <limits.h>

int usage_has_values(UsageStats usage) {
  return usage.prompt_tokens > 0 || usage.completion_tokens > 0 ||
         usage.total_tokens > 0 || usage.context_limit > 0;
}

static void format_count(int value, char *buf, size_t buf_size) {
  if (value >= 1000000) {
    snprintf(buf, buf_size, "%.1fm", value / 1000000.0);
  } else if (value >= 10000) {
    snprintf(buf, buf_size, "%.1fk", value / 1000.0);
  } else if (value >= 1000) {
    snprintf(buf, buf_size, "%.1fk", value / 1000.0);
  } else {
    snprintf(buf, buf_size, "%d", value);
  }
}

int usage_format(UsageStats usage, char *buf, size_t buf_size) {
  if (!buf || buf_size == 0)
    return 0;

  buf[0] = '\0';
  if (!usage_has_values(usage))
    return 0;

  if (usage.context_limit > 0) {
    /* Prefer the provider aggregate. Legacy records without a total use the
     * components; never accumulate inputs from previous requests. */
    long long occupied = usage.total_tokens > 0 ? usage.total_tokens :
        (long long)usage.prompt_tokens + usage.completion_tokens;
    int used = occupied > INT_MAX ? INT_MAX : (int)occupied;

    char used_buf[16];
    char limit_buf[16];
    format_count(used, used_buf, sizeof(used_buf));
    format_count(usage.context_limit, limit_buf, sizeof(limit_buf));

    long long pct = ((long long)used * 100 + usage.context_limit / 2) / usage.context_limit;
    int n = snprintf(buf, buf_size, " %s/%s %lld%% ", used_buf, limit_buf, pct);
    if (n < 0 || (size_t)n >= buf_size) {
      buf[buf_size - 1] = '\0';
      return 0;
    }
    return n;
  }

  char prompt[16];
  char completion[16];
  format_count(usage.prompt_tokens, prompt, sizeof(prompt));
  format_count(usage.completion_tokens, completion, sizeof(completion));

  int n = snprintf(buf, buf_size, "tok in %s out %s", prompt, completion);
  if (n < 0 || (size_t)n >= buf_size) {
    buf[buf_size - 1] = '\0';
    return 0;
  }
  return n;
}
