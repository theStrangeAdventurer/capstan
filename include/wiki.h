#ifndef WIKI_H
#define WIKI_H

typedef struct WikiCache WikiCache;

char *wiki_expand_path(const char *path);
char *wiki_build_prompt(const char *wiki_path);
char *wiki_build_summary(const char *wiki_path);

/* Cached variants: identical output, but the wiki tree is probed with stat
 * only and re-rendered exclusively when files were added, removed or changed
 * (mtime/size). A NULL cache degrades to the uncached builders. */
WikiCache *wiki_cache_create(void);
void wiki_cache_free(WikiCache *cache);
char *wiki_build_prompt_cached(const char *wiki_path, WikiCache *cache);
char *wiki_build_summary_cached(const char *wiki_path, WikiCache *cache);

#endif
