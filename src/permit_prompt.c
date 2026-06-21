#include "permit_prompt.h"
#include "popup_internal.h"

static void clamp_choice(int *choice) {
  if (*choice < PERMIT_CHOICE_YES)
    *choice = PERMIT_CHOICE_YES;
  else if (*choice > PERMIT_CHOICE_ALWAYS)
    *choice = PERMIT_CHOICE_ALWAYS;
}

PermitPromptAction permit_prompt_handle_key(int ch, int *choice) {
  clamp_choice(choice);

  switch (ch) {
  case POPUP_KEY_LEFT:
  case POPUP_KEY_UP:
  case 'h':
  case 'k':
    if (*choice > PERMIT_CHOICE_YES)
      (*choice)--;
    break;
  case POPUP_KEY_RIGHT:
  case POPUP_KEY_DOWN:
  case 'l':
  case 'j':
    if (*choice < PERMIT_CHOICE_ALWAYS)
      (*choice)++;
    break;
  case 'y':
  case 'Y':
    *choice = PERMIT_CHOICE_YES;
    return PERMIT_PROMPT_DONE;
  case 'n':
  case 'N':
    *choice = PERMIT_CHOICE_NO;
    return PERMIT_PROMPT_DONE;
  case 'a':
  case 'A':
    *choice = PERMIT_CHOICE_ALWAYS;
    return PERMIT_PROMPT_DONE;
  case '\t':
  case '\n':
  case '\r':
    return PERMIT_PROMPT_DONE;
  case 27:
    *choice = PERMIT_CHOICE_NO;
    return PERMIT_PROMPT_DONE;
  }

  return PERMIT_PROMPT_CONTINUE;
}

const char *permit_prompt_result(int choice) {
  switch (choice) {
  case PERMIT_CHOICE_YES:
    return "allow";
  case PERMIT_CHOICE_ALWAYS:
    return "always";
  default:
    return "deny";
  }
}
