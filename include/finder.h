#ifndef FINDER_H
#define FINDER_H

#include "popup.h"

typedef struct {
  char *pattern;
  int negated;
  int anchored;
  int dir_only;
  int has_slash;
} FinderIgnorePattern;

typedef struct {
  FinderIgnorePattern *items;
  int count;
  int capacity;
} FinderIgnoreList;

void finder_ignore_init(FinderIgnoreList *list);
void finder_ignore_free(FinderIgnoreList *list);
int finder_ignore_add(FinderIgnoreList *list, const char *pattern);
int finder_ignore_load_file(FinderIgnoreList *list, const char *root,
                            const char *filename);
int finder_path_ignored(const FinderIgnoreList *list, const char *rel_path,
                        int is_dir);
int finder_collect_files(const char *root, const FinderIgnoreList *ignore,
                         PopupItem **out_items, int *out_count);

int finder_fuzzy_score(const char *text, const char *query);

#endif
