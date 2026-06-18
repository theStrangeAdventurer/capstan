#!/bin/sh
set -eu

out=$1
shift

tmp="${out}.tmp"
mkdir -p "$(dirname "$out")"

{
  printf '#include "embedded_assets.h"\n'
  printf '#include <stddef.h>\n'
  printf '#include <string.h>\n\n'

  i=0
  for path in "$@"; do
    printf 'static const char embedded_asset_%d[] =\n' "$i"
    awk '{
      gsub(/\\/, "\\\\")
      gsub(/"/, "\\\"")
      printf "  \"%s\\n\"\n", $0
    }' "$path"
    printf '  ;\n\n'
    i=$((i + 1))
  done

  printf 'static const EmbeddedAsset g_embedded_assets[] = {\n'
  i=0
  for path in "$@"; do
    printf '  {"%s", embedded_asset_%d, sizeof(embedded_asset_%d) - 1},\n' "$path" "$i" "$i"
    i=$((i + 1))
  done
  printf '};\n\n'

  printf 'const EmbeddedAsset *embedded_assets(size_t *count) {\n'
  printf '  if (count)\n'
  printf '    *count = sizeof(g_embedded_assets) / sizeof(g_embedded_assets[0]);\n'
  printf '  return g_embedded_assets;\n'
  printf '}\n\n'

  printf 'const EmbeddedAsset *embedded_asset_find(const char *path) {\n'
  printf '  size_t count = 0;\n'
  printf '  const EmbeddedAsset *assets = embedded_assets(&count);\n'
  printf '  for (size_t i = 0; i < count; i++) {\n'
  printf '    if (strcmp(assets[i].path, path) == 0)\n'
  printf '      return &assets[i];\n'
  printf '  }\n'
  printf '  return NULL;\n'
  printf '}\n'
} > "$tmp"

mv "$tmp" "$out"
