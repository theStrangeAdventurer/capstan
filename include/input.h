#ifndef INPUT_H
#define INPUT_H

#define INPUT_BUFFER_SIZE 8192
/* Each raw byte can at worst become part of a 24-byte paste label. */
#define INPUT_DISPLAY_BUFFER_SIZE (24 * INPUT_BUFFER_SIZE + 192)

#include <stddef.h>

typedef struct {
  char *mime_type;
  char *data;
} InputImage;

void input_init(void);
const char *input_get_text(void);
const char *input_get_display_text(void);
/* History keeps image labels but expands pasted text. Shares display storage. */
const char *input_get_message_text(void);
int input_get_cursor(void);
int input_get_display_cursor(void);
void input_insert(int ch);
/* Incremental bracketed-paste body decoder, shared by both TUI loops. */
void input_set_paste_width(int width);
void input_paste_begin(void);
int input_paste_active(void);
int input_paste_feed(int ch);
void input_set_text(const char *text);
void input_backspace(void);
void input_delete_word_backward(void);
void input_delete_to_line_start(void);
void input_move_left(void);
void input_move_right(void);
void input_clear(void);
int input_add_image(const char *mime_type, const char *base64_data);
size_t input_image_count(void);
InputImage *input_take_images(size_t *count);
void input_images_free(InputImage *images, size_t count);

#endif
