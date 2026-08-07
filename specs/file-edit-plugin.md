# File Edit Plugin

## Behavior

`/edit <path> <old_text> <new_text>` replaces an exact text fragment in an
existing file. The agent tool `file_edit` uses the same implementation with
structured `path`, `old_text`, `new_text`, and optional `replace_all` arguments.

- Relative paths resolve against the configured
  [workspace directory](workspace-directory.md).
- `old_text` must be non-empty.
- If `old_text` is not found, the command fails without writing.
- If `old_text` appears more than once and `replace_all` is not true, the command
  fails without writing.
- If `replace_all` is true, every exact occurrence is replaced.
- Existing UTF-8 BOMs are preserved.
- Successful edits report the resolved path, replacement count, and a unified
  diff showing removed `-` lines and added `+` lines. The diff is returned to
  the model as the tool result and displayed in the agent transcript for
  `file_edit` calls.
- In the TUI transcript, removed diff lines are rendered with a dark red
  background and added diff lines with a dark green background when terminal
  colors are available. The same highlighting applies to Markdown fenced
  `diff` code blocks and remains active across terminal-width soft wraps.

## Rationale

`file_write` is intentionally a full-file write tool. It is appropriate for new
files, explicit overwrites, and appends, but it is unsafe as the primary way to
modify existing files because providing only the desired addition replaces the
whole file.

`file_edit` gives the agent a targeted editing primitive: read the file, choose
a precise existing fragment, and replace only that fragment. Ambiguous or stale
context fails closed instead of corrupting the file.

## Tests

`make test-http-lua` covers single replacement, missing text, ambiguous matches,
`replace_all`, UTF-8 BOM preservation, and diff output in tool results.
