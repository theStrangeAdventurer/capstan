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
  const char *workdir;
  int max_turns;
  int json;
  const char *error;
} CliOptions;

CliOptions cli_options_default(void);
CliOptions cli_parse(int argc, char **argv);

#endif
