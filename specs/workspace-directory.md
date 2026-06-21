# Workspace Directory

## Behavior

Capstan exposes a configured workspace directory to Lua as `capstan.workdir`.
Local tools use it as the base for relative paths and shell commands.

Workspace selection happens at startup:

1. `CAPSTAN_WORKDIR`, when it is an absolute existing directory.
2. `CAPSTAN_WORKSPACE`, when it is an absolute existing directory.
3. The parent project directory inferred from the binary path when the binary is
   inside a repo-style `build/` directory with `src/` and `plugins/` beside it.
4. Launch `PWD`, when it is an absolute existing directory.
5. Process `getcwd()`.
6. `.` as a last-resort fallback.

## Tool Impact

- `file_read` resolves relative files and directories against `capstan.workdir`.
- `file_write` resolves relative output paths against `capstan.workdir`.
- `shell` child processes `chdir(capstan.workdir)` before executing
  `/bin/sh -c <command>`.
- `file_read` permission checks treat `capstan.workdir` as the allowed local
  workspace root.

Manual user commands and model-initiated tool calls share the same workspace
base. This keeps `pwd`, `ls src`, `/file README`, and `file_write RESUME.md`
aligned even when the app process starts from a temporary sandbox directory.

## Tests

`make test-http-lua` covers Lua file tools resolving through `capstan.workdir`.
`make test-build` verifies the standalone binary still embeds and loads the
built-in runtime assets after workspace initialization.
