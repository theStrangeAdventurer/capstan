# Workspace footer

The input box's bottom border shows the actual `app_workdir()` on the left and
workspace VCS changes on the right, on both the start screen and conversation.
No extra terminal row is consumed. The path uses the existing dim border color;
file count uses the normal terminal foreground, additions and deletions use the
existing soft green/red diff colors (65/95 on 256-color terminals).

Example: `~/project/src` on the left, `3 files · +128 −34` on the right.

## Semantics and ownership

- `agent/vcs.lua` owns the shared adapter selection for tools and the footer.
  `src/workspace_status.c` owns the nonblocking worker and cache; `src/tui.c`
  bridges the pure Lua descriptor, not tool execution. Custom adapters provide
  `commands.summary` in the existing VCS config. See [VCS tool](vcs-tool.md)
  for its per-file protocol and automatic-execution contract. Without summary
  support the footer shows `diff unavailable`; it never falls back to Git.
- The following baseline/counting semantics describe the built-in Git adapter;
  custom adapters normalize their own per-file statistics.
- Statistics cover the configured workspace, not just the displayed subdirectory.
  Git commands are scoped with a literal `.` after changing to the workspace.
- File count includes staged/unstaged paths once, renames once, conflicts,
  binary changes and individual untracked files (but not ignored files).
- Line totals are the net HEAD-to-worktree diff, not the sum of staged and
  unstaged diffs. A replaced line contributes one deletion and one addition.
  An unborn repository uses the empty tree in its own object format, without
  writing an object or index. Untracked file contents are not read/countable as
  lines; binaries likewise contribute files, not fictional line totals.
- A clean Git workspace shows `clean`. Initial loading shows just the path.
  Missing Git, command failures, timeout, truncated or malformed output show
  `diff unavailable`, not zero or stale success.

## Refresh and safety

The renderer polls a nonblocking pipe. A dedicated process group collects
metadata using the selected adapter's argv command (no shell interpolation).
The built-in Git command uses an embedded fixed script. Git
external diff/text conversion, fsmonitor and optional index locking are disabled.
Filenames are always quoted by Git so embedded tabs/newlines cannot create fake
records; only aggregate numbers leave the parser. Output is bounded to 1 MiB,
read work per frame is bounded, and a worker times out after five seconds.

A new collection starts two seconds after the previous result, picking up agent
and external edits without scanning on every paint. Only one worker exists;
last successful counts stay visible during normal refresh. Workspace, selected
adapter and summary-command changes cancel the previous worker and clear its cache. Normal exit kills/reaps the
worker group. Blocking UI waits also render/poll, so tool execution does not
freeze the indicator. No background work starts in CLI or ACP modes.

## Responsive layout

`src/tui_layout.c` owns pure layout: HOME is shortened only on path boundaries,
control characters are sanitized, long paths retain trailing components with a
middle ellipsis, and Unicode is measured in terminal cells. On narrow terminals,
file count disappears first; on very narrow ones the summary yields to the path.
Labels never overwrite the corners or overlap each other.

## Tests

`make test` covers parser validation, clean/unborn repositories, staged plus
unstaged edits, renames, binary/deleted/untracked/ignored files, newline filenames,
workspace confinement, cache invalidation, external refresh, error states, HOME
boundaries, Unicode truncation and narrow layout. `make test-tui-input` includes a
real ncurses regression for green/red counters, normal file-count color, visible
workdir, resizing and unchanged model input. `make` verifies UI integration.
