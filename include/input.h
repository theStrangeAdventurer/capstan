#ifndef INPUT_H
#define INPUT_H

#define INPUT_BUFFER_SIZE 8192
#define INPUT_DISPLAY_BUFFER_SIZE (INPUT_BUFFER_SIZE + 192)

#include <stddef.h>

typedef struct {
  char *mime_type;
  char *data;
} InputImage;

void input_init(void);
const char *input_get_text(void);
const char *input_get_display_text(void);
int input_get_cursor(void);
int input_get_display_cursor(void);
void input_insert(int ch);
void input_set_text(const char *text);
void input_backspace(void);
void input_move_left(void);
void input_move_right(void);
void input_clear(void);
int input_add_image(const char *mime_type, const char *base64_data);
size_t input_image_count(void);
InputImage *input_take_images(size_t *count);
void input_images_free(InputImage *images, size_t count);

#endif
