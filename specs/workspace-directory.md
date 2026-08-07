# Working Directory and Workspace Root

## Behavior

Capstan keeps two related paths:

- `capstan.workdir` is the current working directory. Relative file paths and
  shell commands start here.
- `capstan.workspace_root` is the project and permission boundary. Project
  instructions, project skills, model file access, and workspace-scoped
  headless permissions use this root.

The working directory is selected from `--workdir`, `CAPSTAN_WORKDIR`, binary
project inference, launch `PWD`, and process `getcwd()`, in that order. The
workspace root is selected separately from `--workspace`,
`CAPSTAN_WORKSPACE`, the nearest ancestor containing `.git`, and finally the
working directory. An explicit workspace root must be an existing absolute
directory that contains the working directory.

Both paths are canonicalized with `realpath` when possible. Changing the
working directory invalidates the inferred workspace root so it is recomputed.

## Tool Impact

- Relative `file_read`, `file_write`, and `file_edit` paths resolve against the
  working directory, then their real paths are checked against the workspace
  root.
- Shell children `chdir(capstan.workdir)` before executing `/bin/sh -c`.
- Shell permission rules use the workspace root as their stable target.
- `--benchmark` rejects statically visible shell path arguments outside the
  workspace root instead of silently traversing the
  user's home directory. This is a policy guard, not an operating-system
  sandbox; normal interactive shell use still follows the permission flow.
- Project `AGENTS.md` and `.agents/skills` are loaded from the workspace root.
- Every model request states both paths explicitly in its environment section.

This allows a task to run from a nested directory while retaining the repository
root as its instruction, skill, and permission boundary.

## Tests

`make test` covers path selection and distinct workdir/workspace state.
`make test-http-lua` covers the model environment, workspace-scoped shell guard,
file paths, project info, and tool permission targets. `make test-build` verifies
that the standalone embedded runtime exposes both paths.
