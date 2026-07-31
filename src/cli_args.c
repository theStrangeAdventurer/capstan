#include "cli_args.h"
#include <limits.h>
#include <string.h>

CliOptions cli_options_default(void) {
  CliOptions opts = {0};
  opts.mode = CLI_MODE_TUI;
  opts.max_turns = 200;
  return opts;
}

static int is_flag(const char *arg, const char *flag) {
  return arg && strcmp(arg, flag) == 0;
}

static const char *next_value(int argc, char **argv, int *i) {
  if (*i + 1 >= argc)
    return NULL;
  (*i)++;
  return argv[*i];
}

static int is_reasoning_effort(const char *value) {
  return value &&
         (strcmp(value, "none") == 0 || strcmp(value, "minimal") == 0 ||
          strcmp(value, "low") == 0 || strcmp(value, "medium") == 0 ||
          strcmp(value, "high") == 0 || strcmp(value, "xhigh") == 0 ||
          strcmp(value, "max") == 0);
}

static int is_profile(const char *value) {
  return value && (strcmp(value, "fast") == 0 ||
                   strcmp(value, "implement") == 0 ||
                   strcmp(value, "plan") == 0);
}

CliOptions cli_parse(int argc, char **argv) {
  CliOptions opts = cli_options_default();

  if (argc <= 1)
    return opts;

  if (is_flag(argv[1], "--self-test-embedded")) {
    opts.mode = CLI_MODE_SELF_TEST;
    return opts;
  }

  if (is_flag(argv[1], "-h") || is_flag(argv[1], "--help")) {
    opts.mode = CLI_MODE_HELP;
    return opts;
  }

  int start = 1;
  if (strcmp(argv[1], "run") == 0) {
    opts.mode = CLI_MODE_RUN;
    start = 2;
  } else {
    opts.mode = CLI_MODE_ERROR;
    opts.error = "unknown command";
    return opts;
  }

  for (int i = start; i < argc; i++) {
    const char *arg = argv[i];
    if (is_flag(arg, "--prompt")) {
      opts.prompt = next_value(argc, argv, &i);
      if (!opts.prompt)
        opts.error = "--prompt requires a value";
    } else if (is_flag(arg, "--prompt-file")) {
      opts.prompt_file = next_value(argc, argv, &i);
      if (!opts.prompt_file)
        opts.error = "--prompt-file requires a value";
    } else if (is_flag(arg, "--provider")) {
      opts.provider = next_value(argc, argv, &i);
      if (!opts.provider)
        opts.error = "--provider requires a value";
    } else if (is_flag(arg, "--model")) {
      opts.model = next_value(argc, argv, &i);
      if (!opts.model)
        opts.error = "--model requires a value";
    } else if (is_flag(arg, "--profile")) {
      opts.profile = next_value(argc, argv, &i);
      if (!opts.profile)
        opts.error = "--profile requires a value";
      else if (!is_profile(opts.profile))
        opts.error = "--profile must be one of fast, implement, plan";
    } else if (is_flag(arg, "--reasoning-effort") ||
               is_flag(arg, "--effort")) {
      opts.reasoning_effort = next_value(argc, argv, &i);
      if (!opts.reasoning_effort)
        opts.error = "--reasoning-effort requires a value";
      else if (!is_reasoning_effort(opts.reasoning_effort))
        opts.error = "--reasoning-effort must be one of none, minimal, low, medium, high, xhigh, max";
    } else if (is_flag(arg, "--workdir")) {
      opts.workdir = next_value(argc, argv, &i);
      if (!opts.workdir)
        opts.error = "--workdir requires a value";
    } else if (is_flag(arg, "--workspace")) {
      opts.workspace = next_value(argc, argv, &i);
      if (!opts.workspace)
        opts.error = "--workspace requires a value";
    } else if (is_flag(arg, "--session-id")) {
      opts.session_id = next_value(argc, argv, &i);
      if (!opts.session_id)
        opts.error = "--session-id requires a value";
    } else if (is_flag(arg, "--max-turns")) {
      const char *value = next_value(argc, argv, &i);
      if (!value) {
        opts.error = "--max-turns requires a value";
      } else {
        int parsed = 0;
        for (const char *p = value; *p; p++) {
          if (*p < '0' || *p > '9') {
            opts.error = "--max-turns must be a positive integer";
            break;
          }
          if (parsed > (INT_MAX - (*p - '0')) / 10) {
            opts.error = "--max-turns is too large";
            break;
          }
          parsed = parsed * 10 + (*p - '0');
        }
        if (!opts.error && parsed <= 0)
          opts.error = "--max-turns must be a positive integer";
        if (!opts.error)
          opts.max_turns = parsed;
      }
    } else if (is_flag(arg, "--json")) {
      opts.json = 1;
    } else if (is_flag(arg, "--no-mcp")) {
      opts.no_mcp = 1;
    } else if (is_flag(arg, "--no-wiki")) {
      opts.no_wiki = 1;
    } else if (is_flag(arg, "--no-preserve-reasoning")) {
      opts.no_preserve_reasoning = 1;
    } else if (is_flag(arg, "--full-control")) {
      opts.full_control = 1;
    } else if (is_flag(arg, "--benchmark")) {
      opts.benchmark = 1;
      opts.no_mcp = 1;
      opts.no_wiki = 1;
      opts.full_control = 1;
    } else if (is_flag(arg, "-h") || is_flag(arg, "--help")) {
      opts.mode = CLI_MODE_HELP;
    } else {
      opts.error = "unknown run option";
    }

    if (opts.error) {
      opts.mode = CLI_MODE_ERROR;
      return opts;
    }
  }

  if (opts.prompt && opts.prompt_file) {
    opts.mode = CLI_MODE_ERROR;
    opts.error = "use only one of --prompt or --prompt-file";
  }

  return opts;
}
