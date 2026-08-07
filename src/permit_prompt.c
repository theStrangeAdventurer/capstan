#include "permit_prompt.h"
#include "popup_internal.h"

static void clamp_choice(int *choice) {
  if (*choice < PERMIT_CHOICE_ONCE)
    *choice = PERMIT_CHOICE_ONCE;
  else if (*choice > PERMIT_CHOICE_REJECT)
    *choice = PERMIT_CHOICE_REJECT;
}

PermitPromptAction permit_prompt_handle_key(int ch, int *choice) {
  clamp_choice(choice);

  switch (ch) {
  case POPUP_KEY_UP:
  case 'k':
    if (*choice > PERMIT_CHOICE_ONCE)
      (*choice)--;
    break;
  case POPUP_KEY_DOWN:
  case 'j':
    if (*choice < PERMIT_CHOICE_REJECT)
      (*choice)++;
    break;
  case 'y':
  case 'Y':
    *choice = PERMIT_CHOICE_ONCE;
    return PERMIT_PROMPT_DONE;
  case 'a':
  case 'A':
    *choice = PERMIT_CHOICE_ALWAYS;
    return PERMIT_PROMPT_DONE;
  case 't':
  case 'T':
    *choice = PERMIT_CHOICE_TOOL;
    return PERMIT_PROMPT_DONE;
  case 'n':
  case 'N':
    *choice = PERMIT_CHOICE_REJECT;
    return PERMIT_PROMPT_DONE;
  case '\t':
  case '\n':
  case '\r':
    return PERMIT_PROMPT_DONE;
  case 27:
    *choice = PERMIT_CHOICE_REJECT;
    return PERMIT_PROMPT_DONE;
  }

  return PERMIT_PROMPT_CONTINUE;
}

const char *permit_prompt_result(int choice) {
  switch (choice) {
  case PERMIT_CHOICE_ONCE:
    return "allow";
  case PERMIT_CHOICE_ALWAYS:
    return "allow_session";
  case PERMIT_CHOICE_TOOL:
    return "allow_tool_run";
  default:
    return "deny";
  }
}
