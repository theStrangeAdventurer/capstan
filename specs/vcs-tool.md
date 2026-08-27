# VCS tool

## Behavior

Capstan provides one built-in read-only VCS adapter, `git`, and a model tool
named `vcs`. The tool supports `status`, `diff`, and `changes`; `diff` and
`changes` use a path-specific diff when `path` is supplied. Existing paths and
the workspace root are canonicalized before confinement is checked, so symlinks
cannot escape the workspace. Permission prompts authorize that exact path, or
the whole workspace root when no path is supplied, and the handler reuses the
same resolved path. The built-in Git commands disable fsmonitor, external diff,
text conversion, and optional index locking. In a Git repository without an
initial commit, diff operations combine the staged and working-tree diffs so
post-staging edits are not omitted.

`/vcs` opens an adapter picker. The selected adapter is persisted per canonical
workspace root in `state.lua`. Selection precedence is:

1. persisted selection for the workspace;
2. `vcs.default` from config;
3. built-in `git`.

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

## Tests

Tests cover configurable workspace markers, argv execution without shell
expansion, disabled Git execution extensions, and complete unborn-repository
diffs. The embedded build smoke test verifies that the built-in plugin and
runtime module are available from the standalone binary.
