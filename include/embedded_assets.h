#ifndef EMBEDDED_ASSETS_H
#define EMBEDDED_ASSETS_H

#include <stddef.h>

typedef struct {
  const char *path;
  const char *data;
  size_t size;
} EmbeddedAsset;

const EmbeddedAsset *embedded_assets(size_t *count);
const EmbeddedAsset *embedded_asset_find(const char *path);

#endif
