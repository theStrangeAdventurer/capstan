# VCS tool

## Behavior

Capstan provides one built-in read-only VCS adapter, `git`, and a model tool
named `vcs`. The tool supports `status`, `diff`, and `changes`; `diff` and
`changes` use a path-specific diff when `path` is supplied. Existing paths and
the workspace root are canonicalized before confinement is checked, so symlinks
cannot escape the workspace. Permission prompts authorize that exact path, or
the whole workspace root when no path is supplied, and the handler reuses the
same authorized path through dispatcher-owned permission context, never an
internal cache supplied in model arguments. The runtime resolves the requested
path again before execution and fails closed if it is missing, escapes the
workspace, or differs from the authorized path (for example after a symlink
change while awaiting permission). The built-in Git commands disable fsmonitor, external diff,
text conversion, and optional index locking. In a Git repository without an
initial commit, diff operations combine the staged and working-tree diffs so
post-staging edits are not omitted.

`/vcs` opens an adapter picker. The selected adapter is persisted per canonical
workspace root in `state.lua`. Selection precedence is:

1. persisted selection for the workspace;
2. `vcs.default` from config;
3. built-in `git`.

## Tool status

The dispatcher displays the requested operation and scope, for example
`Inspecting VCS: status · workspace — done` or
`Inspecting VCS: diff · src/main.c — done`. Paths are relative to the workspace
root, not the current subdirectory. This display is separate from the canonical
permission target; authorization and execution remain unchanged. Failures show
`— error: <reason>` and permission refusals show `— denied`, never `— done`.
Dispatcher tests cover all three operations, path-specific calls, errors, and
denials (`make test-http-lua`).

## Custom adapters

Adapters are configured as argv arrays, never shell command strings. This keeps
model-provided paths from being interpreted by a shell. `{path}` is replaced
only when it occupies a complete argv element. Every path-specific command must
contain `{path}`; otherwise Capstan rejects the operation rather than granting a
narrow permission for workspace-wide output.

Mercurial example:

```lua
return {
  workspace = {
    markers = { ".hg" },
  },
  vcs = {
    default = "hg",
    adapters = {
      hg = {
        label = "Mercurial",
        commands = {
          status = { "hg", "status" },
          diff = { "hg", "diff" },
          diff_path = { "hg", "diff", "--", "{path}" },
        },
      },
    },
  },
}
```

Configuration is owner-trusted. Capstan guarantees that the model can select
only a declared operation; adapter authors are responsible for keeping declared
commands read-only. Adapter failures and unsupported operations are returned as
failed model tool calls rather than successful result payloads.

## Shared UI statistics

The same `agent.vcs.current()` selection now owns tool status/diffs (including
`diff_path`) and the footer's file count and `+ / −` line totals. No separate UI
VCS setting exists. Add an optional argv command to the same adapter:

```lua
commands = {
  status = { "hg", "status" },
  diff = { "hg", "diff" },
  diff_path = { "hg", "diff", "--", "{path}" },
  summary = { "python3", "/absolute/path/to/my-hg-summary.py" },
}
```

The helper is supplied by the adapter author; `hg diff --stat` is not this
protocol. `summary` receives no model input or `{path}` substitution. It runs
with the configured workspace as cwd, stdin closed, every two seconds after
completion while the TUI is visible. Declaring it opts into automatic local
execution without a permission prompt: keep it read-only, bounded, and offline.
CLI/ACP do not collect footer statistics. Existing adapters without `summary`
keep working for tools but show `diff unavailable`, never a Git fallback.
Overriding the `git` adapter also replaces its summary capability.

### `files-v1` output contract

Return exit 0 and UTF-8 stdout, with literal tabs between fields:

```text
files-v1
M\t12\t3\tsrc/main.c
R\t0\t0\tnew-name.c
?\t-\t-\tuntracked.txt
M\t-\t-\timage.png
done
```

Each row is `status<TAB>added<TAB>deleted<TAB>path<LF>`. Status is one of
`M A D R C U T ?`. Paths are nonempty workspace-relative display labels;
escape embedded controls (for example newline as `\\n`, tab as `\\t`) and
backslashes yourself. Emit each changed path once, including renames once at
the destination. Counts are unsigned decimal integers; `-` means uncounted
(binary or untracked contents), contributing zero to line totals but one file.
Use net changes against the VCS baseline, not staged plus unstaged totals.
The footer aggregates the per-file rows; adapters own VCS-specific semantics.
A clean workspace returns `files-v1\ndone\n`; no repository returns `none\n`.
Both require the final LF. Nonzero exit, missing/truncated framing, control
characters in paths, NUL bytes, numeric overflow, output over 1 MiB, and a
five-second timeout fail closed as `diff unavailable`.

The built-in Git adapter declares an embedded `agent/vcs_git_stats.sh` collector
and uses the existing quoted Git status/numstat framing for compatibility. This
is an internal adapter format, not a requirement for custom VCS implementations.
`src/workspace_status.c` owns only process execution, parsing and caching;
`src/tui.c` bridges the pure Lua descriptor without running model tools.
Changing selection, workspace or command cancels stale work and clears counts.
No footer data enters model context, logs or persisted messages. The existing
`file_edit` preview remains an exact before/after edit, not a repository diff;
repository diffs continue through the selected adapter's `diff`/`diff_path`.

## Tests

Tests cover configurable workspace markers, argv execution without shell
expansion, disabled Git execution extensions, and complete unborn-repository
diffs. Dispatcher tests cover forged internal path arguments, missing and
out-of-workspace paths, explicit denies, valid path-specific diffs, and symlink
changes after authorization. The embedded build smoke test verifies that the built-in plugin and
runtime module are available from the standalone binary.
