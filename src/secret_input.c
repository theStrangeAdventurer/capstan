#include "secret_input.h"

void secret_input_clear(SecretInput *input) {
  volatile unsigned char *p = (volatile unsigned char *)input;
  for (size_t i = 0; i < sizeof(*input); i++) p[i] = 0;
}

static void reject(SecretInput *input) {
  int paste = input->paste;
  secret_input_clear(input);
  input->paste = paste;
  input->invalid = 1;
}

int secret_input_feed(SecretInput *s, int byte) {
  if (byte == -1)
    return s->escape && !s->paste ? SECRET_CANCEL : SECRET_MORE;
  if (s->escape) {
    const char *marker = s->paste ? "\033[201~" : "\033[200~";
    if (byte == marker[s->escape]) {
      if (++s->escape == 6) {
        s->escape = 0;
        s->paste = !s->paste;
      }
      return SECRET_MORE;
    }
    if (!s->paste) return SECRET_CANCEL;
    reject(s);
    /* An overlapping ESC may begin the real paste terminator. */
    if (byte == 27) s->escape = 1;
    return SECRET_MORE;
  }
  if (byte == 27) {
    s->escape = 1;
    return SECRET_MORE;
  }
  if (!s->paste && (byte == '\r' || byte == '\n')) {
    if (s->invalid) {
      secret_input_clear(s);
      return SECRET_MORE;
    }
    return s->length ? SECRET_SUBMIT : SECRET_MORE;
  }
  if (!s->paste && (byte == 127 || byte == 8)) {
    if (s->invalid) secret_input_clear(s);
    else if (s->length) s->text[--s->length] = 0;
    return SECRET_MORE;
  }
  /* API keys are printable ASCII. Reject rather than silently altering them. */
  if (byte < 32 || byte > 126 || s->length == SECRET_INPUT_MAX) {
    reject(s);
    return SECRET_MORE;
  }
  if (!s->invalid) s->text[s->length++] = (char)byte;
  return SECRET_MORE;
}
