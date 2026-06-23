#ifndef SKILLS_H
#define SKILLS_H

char *skills_build_prompt(const char *builtin_skills_dir,
                          const char *project_skills_dir,
                          const char *user_skills_dir,
                          const char *common_skills_dir);
char *skills_build_summary(const char *builtin_skills_dir,
                           const char *project_skills_dir,
                           const char *user_skills_dir,
                           const char *common_skills_dir);

#endif
