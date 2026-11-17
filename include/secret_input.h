#ifndef SECRET_INPUT_H
#define SECRET_INPUT_H
#include <stddef.h>
#define SECRET_INPUT_MAX 8192
/* Private modal state; never share with the ordinary input/history buffer. */
typedef struct {
  char text[SECRET_INPUT_MAX + 1];
  size_t length;
  int paste, escape, invalid;
} SecretInput;
enum { SECRET_MORE, SECRET_SUBMIT, SECRET_CANCEL };
void secret_input_clear(SecretInput *input);
/* Raw bytes; -1 is an idle timeout (disambiguates standalone Escape). */
int secret_input_feed(SecretInput *input, int byte);
#endif
