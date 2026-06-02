#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @see https://www.youtube.com/watch?v=95M6V3mZgrI
 */
#define da_append(arr, item)                                                   \
  do {                                                                         \
    if ((arr)->size >= (arr)->capacity) {                                      \
      if (!(arr)->capacity)                                                    \
        (arr)->capacity = 256;                                                 \
      size_t temp_cap = (arr)->capacity * 2;                                   \
      typeof(*(arr)->items) *temp_items =                                      \
          realloc((arr)->items, temp_cap * sizeof(*(arr)->items));             \
      if (!temp_items) {                                                       \
        fputs("da_append: realloc failed\n", stderr);                          \
        abort();                                                               \
      }                                                                        \
      (arr)->items = temp_items;                                               \
      (arr)->capacity = temp_cap;                                              \
    }                                                                          \
    (arr)->items[(arr)->size++] = item;                                        \
  } while (0)


#define da_free_each(arr, fn)                                                  \
  do {                                                                         \
    for (size_t _i = 0; _i < (arr)->size; ++_i)                                \
      fn((arr)->items[_i]);                                                    \
    free((arr)->items);                                                        \
    (arr)->items = NULL;                                                       \
    (arr)->size = (arr)->capacity = 0;                                         \
  } while (0)


