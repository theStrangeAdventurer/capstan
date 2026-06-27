#ifndef SKILLS_H
#define SKILLS_H

#include <stddef.h>

typedef struct {
  const char *name;
  const char *path;
  const char *content;
  size_t content_size;
} BuiltinSkill;

char *skills_build_prompt(const BuiltinSkill *builtin_skills,
                          size_t builtin_skill_count,
                          const char *project_skills_dir,
                          const char *user_skills_dir,
                          const char *common_skills_dir);
char *skills_build_summary(const BuiltinSkill *builtin_skills,
                           size_t builtin_skill_count,
                           const char *project_skills_dir,
                           const char *user_skills_dir,
                           const char *common_skills_dir);

#endif
