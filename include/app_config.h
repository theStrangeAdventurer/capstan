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
int app_state_dir(char *buf, size_t buf_size);
int app_state_path(char *buf, size_t buf_size, const char *relative_path);
int app_state_ensure_dir(void);
void app_workdir_init(const char *argv0);
int app_workdir_set(const char *path);
const char *app_workdir(void);

#endif
