# Project Instructions

Capstan adds user- and project-level `AGENTS.md` instructions to agent context.

## Behavior

At startup, non-isolated runs load instructions in this order:

1. `~/.config/capstan/AGENTS.md` when it exists;
2. otherwise `~/.agents/AGENTS.md` as the shared-user fallback;
3. `<workspace-root>/AGENTS.md` when it exists.

The Capstan-specific and shared-user files are alternatives, not cumulative.
Project instructions are appended after user instructions. Every section
includes the absolute source path so the model and user can identify the active
policy.

The workspace root is `app_workspace_root()`. It may be an ancestor of the
current `app_workdir()` used for relative file and shell operations. Missing or
unreadable instruction files are ignored without preventing startup.

`capstan run --benchmark` uses isolated runtime mode and loads none of these
files.

## Architecture

`src/project_instructions.c` owns file precedence, loading, and prompt
formatting as dependency-free C logic. `src/plugins_runtime.c` appends that
prompt after the embedded or user-overridden base prompt and before the skill
index and Wiki context.

The precedence mirrors OpenCode's global-then-project policy while using
Capstan's own config and shared-agent directories.

## Constraints

- Instruction files are loaded once at startup. Restart Capstan after editing
  them.
- Capstan currently loads the workspace-root project file; nested instruction
  discovery during individual file reads is not implemented.

## Tests

`make test` covers Capstan-specific user precedence, the shared-user fallback,
and user-before-project ordering. `make test-build` verifies isolated standalone
startup without nearby instruction files.
