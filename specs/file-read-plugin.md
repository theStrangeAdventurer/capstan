# File Read Plugin

## Behavior

`/file <path...>` reads files or lists directories and adds the result to the
conversation context. The agent tool `file_read` accepts either
`{ "path": "..." }` or `{ "paths": ["...", "..."] }` and uses the same
path behavior. If both are present, `paths` entries are read first and `path`
is appended, with duplicates removed.

The published schema intentionally avoids `anyOf`/`oneOf` because some supported
providers reject composition keywords in tool schemas. `minProperties = 1`
together with `additionalProperties = false` still requires at least one known
argument, while runtime validation rejects empty or unusable values. Allowing
both known fields is the compatibility trade-off.

- `paths` batches ordinary workspace reads into one model tool call. Entries
  are de-duplicated while preserving their order. The dispatcher checks each
  disk path individually (including an appended `path`) before the handler
  reads anything. A permission denial rejects the entire call, with no partial
  reads. Target/session grants retain the same meaning as for a single read.
  A batch is never replaced by a singular Wiki read.
- Batch reads deliberately reject sensitive paths; use singular `path` for
  those files. External paths are supported in either form, with a separate
  permission decision for every disk target before any reads.

- Missing paths return `Usage: /file <filename...>`.
- Absolute paths are used as provided.
- Relative paths resolve against the configured
  [workspace directory](workspace-directory.md).
- Paths with the `embedded:` prefix read read-only embedded runtime assets
  through `capstan.embedded_asset`. This is used for built-in skill files such
  as `embedded:skills/wiki-onboarding/SKILL.md` and does not touch the
  filesystem. The shared `workspace.embedded_asset_name` parser identifies
  these references for the reader and dispatcher; they must not be normalized
  against the workspace or routed into Wiki reads. Tool logs, UI status, and
  model history preserve the literal `embedded:` reference. Embedded reads
  skip filesystem permission checks and prompts, individually and in batches;
  this grants no access to disk. Actions described by an embedded skill still
  require their own normal permissions.
- Missing, empty, or unavailable embedded assets return an embedded-read error,
  without falling back to disk reads or directory listings. Tool descriptions
  explicitly tell the model how to read these references.
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
  inserted into a JSON text field. This also applies to manual `/file` selection:
  images stay with the buffered context badge until the next submission, then
  become structured user-message attachments and persist with the session.
- Other binary files return a short size description instead of raw bytes, so
  invalid UTF-8 cannot corrupt the next provider request.

## Tool status

The dispatcher owns activity labels separately from permission targets. A read
shows the actual requested paths in execution order, de-duplicated using the
same helper as the reader and permission checks. Paths inside the current
working directory are shown relatively; external paths remain absolute and
`embedded:` references remain literal. A batch never displays a fabricated
`<workdir>/file_read` path.

The status ends with `— done`, `— denied`, or `— error: <reason>`. Missing
arguments and reader failures (including partial batch failures) are failed
tool calls, not successful calls containing error text. The first reader error
is used for the short UI reason, while all batch results remain in model
context. File contents cannot determine success or failure. Manual `/file`
output is unchanged. Silent tool runs still suppress these UI messages.

Dispatcher tests cover single/batch paths, external and embedded references,
empty arguments, denial before any reads, partial failures, error-like file
contents, and silent execution (`make test-http-lua`).

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
directory listing. Dispatcher regression tests verify embedded references in
logs, UI, and model history, no filesystem or permission access (including when
Wiki overlaps the workspace), and missing/unavailable/empty asset errors.
Batch tests cover per-path denial (also under YOLO), prompts, target grants,
deduplication, mixed disk/embedded reads, and combined `paths`/`path` arguments.
