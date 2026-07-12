#ifndef WIKI_H
#define WIKI_H

char *wiki_expand_path(const char *path);
char *wiki_build_prompt(const char *wiki_path);
char *wiki_build_summary(const char *wiki_path);

#endif
