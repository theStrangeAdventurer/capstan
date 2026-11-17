/* Standalone: cc -Iinclude src/secret_input.c test/test_secret_input.c -o build/test_secret_input */
#include "secret_input.h"
#include <assert.h>
#include <string.h>

static void feed(SecretInput *s, const char *text) {
  while (*text) assert(secret_input_feed(s, (unsigned char)*text++) == SECRET_MORE);
}
int main(void) {
  SecretInput s = {0};
  feed(&s, "fake-keyX\177");
  assert(!strcmp(s.text, "fake-key"));
  assert(secret_input_feed(&s, '\r') == SECRET_SUBMIT);
  secret_input_clear(&s);
  feed(&s, "\033[200~fake-key\033[201~");
  assert(!s.paste && !s.invalid && !strcmp(s.text, "fake-key"));
  assert(secret_input_feed(&s, '\n') == SECRET_SUBMIT);
  secret_input_clear(&s);
  feed(&s, "\033[200~fake\r\nkey\033[201~");
  assert(s.invalid && !s.paste && !s.length);
  assert(secret_input_feed(&s, '\r') == SECRET_MORE);
  assert(!s.invalid && !s.length);
  feed(&s, "\033[200~fake\033[2bad\033[201~");
  assert(s.invalid && !s.paste);
  secret_input_clear(&s);
  for (int i = 0; i < SECRET_INPUT_MAX; i++) feed(&s, "x");
  assert(s.length == SECRET_INPUT_MAX);
  feed(&s, "x");
  assert(s.invalid && !s.length);
  secret_input_clear(&s);
  feed(&s, "\033");
  assert(secret_input_feed(&s, -1) == SECRET_CANCEL);
  secret_input_clear(&s);
  feed(&s, "\033[200~fake\033");
  assert(secret_input_feed(&s, -1) == SECRET_MORE);
  feed(&s, "[201~");
  assert(!s.paste && !strcmp(s.text, "fake"));
  secret_input_clear(&s);
  feed(&s, "fake\tkey");
  assert(s.invalid && !s.length);
  secret_input_clear(&s);
  for (size_t i = 0; i < sizeof(s); i++) assert(((unsigned char *)&s)[i] == 0);
  return 0;
}
