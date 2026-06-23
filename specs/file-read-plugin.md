# File Read Plugin

## Behavior

`/file <path...>` reads files or lists directories and adds the result to the
conversation context. The agent tool `file_read` uses the same path behavior.

- Missing paths return `Usage: /file <filename...>`.
- Absolute paths are used as provided.
- Relative paths resolve against the configured
  [workspace directory](workspace-directory.md).
- `README` falls back to common README extensions when the exact file is
  missing.
- Manual directory paths are listed with one entry per line and directories
  suffixed with `/`.
- Directory listing shell arguments are single-quote escaped and passed after
  `--` so path text cannot become additional shell commands or options.

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
workspace-relative file reads, and shell-quoting regression for directory
listing.
