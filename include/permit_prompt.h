#ifndef PERMIT_PROMPT_H
#define PERMIT_PROMPT_H

typedef enum {
  PERMIT_CHOICE_YES = 0,
  PERMIT_CHOICE_NO = 1,
  PERMIT_CHOICE_TOOL_RUN = 2,
  PERMIT_CHOICE_FULL_RUN = 3,
  PERMIT_CHOICE_ALWAYS = 4
} PermitChoice;

typedef enum {
  PERMIT_PROMPT_CONTINUE = 0,
  PERMIT_PROMPT_DONE = 1
} PermitPromptAction;

PermitPromptAction permit_prompt_handle_key(int ch, int *choice);
const char *permit_prompt_result(int choice);

#endif
