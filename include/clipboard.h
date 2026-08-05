#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <stddef.h>

#define CLIPBOARD_IMAGE_MAX_BYTES (10u * 1024u * 1024u)

typedef enum {
  CLIPBOARD_READ_OK = 0,
  CLIPBOARD_READ_EMPTY,
  CLIPBOARD_READ_OVERFLOW,
  CLIPBOARD_READ_ERROR
} ClipboardReadStatus;

/* Reads at most limit bytes and never retains bytes beyond that limit. */
ClipboardReadStatus clipboard_read_fd_limited(int fd, size_t limit,
                                               unsigned char **data,
                                               size_t *size);

/* Returns malloc-owned image bytes, or NULL with a user-facing error. */
unsigned char *clipboard_read_image(size_t *size, char *error,
                                    size_t error_size);
char *clipboard_base64_encode(const unsigned char *data, size_t size);

#endif
