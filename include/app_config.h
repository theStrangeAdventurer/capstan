#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stddef.h>

extern const char *APP_NAME;
extern const char *APP_BINARY_NAME;
extern const char *APP_CONFIG_DIR_NAME;
extern const char *APP_EDITOR_TEMP_TEMPLATE;
extern const char *APP_BANNER_TITLE;
extern const char *APP_BANNER_TAGLINE;

int app_config_dir(char *buf, size_t buf_size);
int app_config_path(char *buf, size_t buf_size, const char *relative_path);
int app_config_ensure_dir(void);

#endif
