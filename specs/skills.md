# Skills

Capstan loads reusable agent instructions from project and user skill
directories at startup.

## Behavior

- Project skills are read from `.agents/skills/` under the active workspace.
- Shared home skills are read from `~/.agents/skills/`.
- User skills are read from `~/.config/capstan/skills/`.
- Gated built-in skills are loaded from embedded binary assets and listed as
  source `builtin` with `embedded:` skill paths.
- A skill must be a directory containing `SKILL.md`, such as
  `skills/code-review/SKILL.md`.
- Direct markdown files like `skills/debug.md` are ignored.
- Directories without `SKILL.md` are ignored; `skill.md` and `README.md` are not
  fallback entrypoints.
- Hidden entries are ignored.
- The skill name is the directory name.
- Only `name` and `description` from the `SKILL.md` YAML FrontMatter are added
  to the system prompt, along with the `SKILL.md` path and source.
- The `SKILL.md` body is not added to the system prompt. The agent must read the
  file path when the FrontMatter indicates the skill is relevant.
- The system prompt states this as a mandatory rule: if the user names a skill
  or the task matches a skill description, the agent must read the listed
  `Skill file` completely before applying the skill. The metadata index alone is
  not enough to use a skill.
- Files below the skill directory, such as `references/*.md` and `scripts/*`,
  are not listed in the system prompt. They are shown by `/skills` for
  diagnostics and should be read only when `SKILL.md` asks for them.
- If multiple directories define the same skill name, later sources override
  earlier sources. Priority from highest to lowest is project `.agents/skills/`,
  Capstan user `~/.config/capstan/skills/`, then shared home
  `~/.agents/skills/`, then built-in gated skills.
- The `self-improvement` built-in skill is disabled unless
  `capabilities.self_improvement = true` is present in
  `~/.config/capstan/config.lua`.
  When enabled, Capstan reads its embedded `SKILL.md` from memory as
  `embedded:skills/self-improvement/SKILL.md` and includes it in the skill scan.
- The lightweight skill index is appended to the Lua `system_prompt` global, so
  every provider can discover available skills without loading their full text.
- `/skills` shows the loaded skill list, including each skill source,
  `SKILL.md` path, resource root, and resource paths.

## Architecture

`src/skills.c` owns filesystem scanning, FrontMatter extraction, recursive
resource discovery for `/skills`, and prompt rendering. It is independent of
Lua, ncurses, and curl, which keeps the behavior covered by unit tests.

`src/plugins.c` appends the rendered skills block while loading
`system_prompt`, after the embedded or user-overridden base prompt has been
selected. It also publishes a human-readable summary as
`capstan.skills_summary`; the built-in `plugins/skills.lua` command displays
that value.

## Constraints

- Skills are loaded once at process startup. Editing a skill requires restarting
  Capstan before the updated instructions are sent to the model.
- Built-in gated skill files are exposed from embedded assets at startup.
  User or project skills can still override them by defining the same skill
  directory name.
- Full `SKILL.md` contents and resource files are not included until the agent
  reads them explicitly.

## Test Notes

`make test` covers FrontMatter-only prompt rendering, ignored fallback formats,
recursive resource manifests for `/skills`, empty directories, shared home and
built-in skill loading, and override behavior.
`make test-http-lua` covers the `/skills` Lua command.
