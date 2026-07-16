# Project Instructions

Capstan automatically adds project-level instructions from `AGENTS.md` to the
agent context.

## Behavior

- At startup, Capstan looks for `AGENTS.md` in the active workspace root.
- The active workspace root is `app_workspace_root()`. It may be an ancestor of
  the current `app_workdir()` used for relative file and shell operations.
- When the file exists, its full contents are appended to `system_prompt` under
  a `Project Instructions` section that includes the absolute file path.
- If the file is missing or unreadable, startup continues without project
  instructions.

## Architecture

`src/plugins.c` loads project instructions while building the Lua
`system_prompt`, after selecting the embedded or user-overridden base prompt and
before appending the lightweight skill index.

## Constraints

- `AGENTS.md` is loaded once at startup. Editing it requires restarting Capstan
  before the new instructions affect model requests.
- Only the workspace-root `AGENTS.md` is loaded. Parent directories and nested
  files are not searched.

## Test Notes

The behavior is covered by the main application build path because it is part of
runtime prompt assembly. `make test-build` verifies startup still succeeds when
no `AGENTS.md` is present beside the copied standalone binary.
