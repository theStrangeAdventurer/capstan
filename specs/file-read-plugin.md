# File Read Plugin

## Behavior

`/file <path...>` reads files or lists directories and adds the result to the
conversation context. The agent tool `file_read` accepts either
`{ "path": "..." }` or `{ "paths": ["...", "..."] }` and uses the same
path behavior.

- `paths` batches ordinary workspace reads into one model tool call. Entries
  are de-duplicated while preserving their order.
- Batch reads deliberately reject sensitive paths and paths outside the
  workspace. Use the singular `path` form for those paths so the existing
  target-specific permission prompt remains in force.

- Missing paths return `Usage: /file <filename...>`.
- Absolute paths are used as provided.
- Relative paths resolve against the configured
  [workspace directory](workspace-directory.md).
- Paths with the `embedded:` prefix read read-only embedded runtime assets
  through `capstan.embedded_asset`. This is used for built-in skill files such
  as `embedded:skills/wiki-onboarding/SKILL.md` and does not touch the
  filesystem.
- `README` falls back to common README extensions when the exact file is
  missing.
- Manual directory paths are listed with one entry per line and directories
  suffixed with `/`.
- Directory paths must return a directory listing even on platforms where
  `io.open(path, "r")` succeeds for directories but reading from the handle
  returns no file content.
- Directory listing shell arguments are single-quote escaped and passed after
  `--` so path text cannot become additional shell commands or options.
- PNG, JPEG, GIF, and WebP files are detected from their file signatures and
  returned to the agent as typed image content. Their raw bytes are never
  inserted into a JSON text field.
- Other binary files return a short size description instead of raw bytes, so
  invalid UTF-8 cannot corrupt the next provider request.

## Finder Popup

`/file<Tab>` opens a filterable file finder popup instead of a directory
drill-down browser.

- The popup has its own `Find:` input line.
- Printable keys typed while the popup is active update the finder query rather
  than the main input line.
- Backspace edits the finder query.
- Up/down arrows and `Ctrl+p`/`Ctrl+n` move the current result.
- Enter selects the current file and passes its resolved path to `/file`.
- Esc cancels the popup.
- Finder result text is relative to the workspace; selected values are resolved
  paths.

Finder scans files recursively under the active workspace. It ignores `.git`
internally, reads workspace-root `.gitignore` by default, and also applies
`finder.ignore_files` and `finder.ignore_patterns` from
[config](config.md).

## Tests

`make test` covers finder matching, ignore rules, filterable popup input, and
selection behavior. `make test-http-lua` covers README fallback,
workspace-relative file reads, model-tool `ctx.tool_args.path` handling,
embedded asset reads, directory path listing, and shell-quoting regression for
directory listing.
