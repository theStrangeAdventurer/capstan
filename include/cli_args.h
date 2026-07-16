#ifndef CLI_ARGS_H
#define CLI_ARGS_H

typedef enum {
  CLI_MODE_TUI,
  CLI_MODE_RUN,
  CLI_MODE_SELF_TEST,
  CLI_MODE_HELP,
  CLI_MODE_ERROR,
} CliMode;

typedef struct {
  CliMode mode;
  const char *prompt;
  const char *prompt_file;
  const char *provider;
  const char *model;
  const char *profile;
  const char *reasoning_effort;
  const char *workdir;
  const char *workspace;
  int max_turns;
  int json;
  int no_mcp;
  int no_wiki;
  int full_control;
  int benchmark;
  const char *error;
} CliOptions;

CliOptions cli_options_default(void);
CliOptions cli_parse(int argc, char **argv);

#endif
