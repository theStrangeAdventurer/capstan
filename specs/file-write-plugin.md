# File Write Plugin

## Behavior

`/write <path> <content>` writes content to a local file. The agent tool
`file_write` uses the same implementation with structured `path` and `content`
arguments.

- Missing path returns `Usage: /write <path> <content>`.
- Missing content writes an empty file.
- Absolute paths are used as provided.
- Relative paths are resolved against the configured
  [workspace directory](workspace-directory.md).
- Successful writes report the resolved path, byte count, and line count.
- Failed writes report the resolved path and the underlying `io.open` error.

## Rationale

Some launchers can start the process in a temporary runtime directory. Lua file
APIs write relative paths against the process cwd, which can make successful
writes appear to disappear from the user's project directory.

The runtime exposes `capstan.workdir` to Lua so `/write SEC_CHECK.md` resolves
against the intended workspace instead of a sandbox temp directory. Reporting
the resolved path makes the destination auditable.

## Tests

`make test-http-lua` covers the Lua plugin. The file write tests verify both
the `PWD` fallback and the higher-priority `capstan.workdir` path when `PWD`
points at a different temporary directory.
