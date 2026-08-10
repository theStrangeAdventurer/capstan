# Optional Promptfoo report

Promptfoo is a presentation and orchestration layer around the canonical
`../scripts/run_eval.py` harness. The harness remains responsible for fixed
task selection, clean agent workspaces, solution-only scoring copies, upstream
tests, process timeouts, and resource measurements.

This layer is optional. Use the direct runner for the simplest reproducible
benchmark; use Promptfoo when an HTML report and its interactive viewer are
useful.

## Prepare a local config

Copy `config.example.yaml` to the ignored `config.local.yaml` in this directory,
fill every placeholder with absolute local paths, and do not commit it. Promptfoo
resolves `file://` adapters next to the config, so the local config must remain
in this directory. It must provide:

- the pinned corpus prepared by `../bootstrap.sh`;
- a private directory for task logs and scoring worktrees;
- the Capstan binary built from the revision under test;
- the repository path for the OpenCode adapter.

Both providers must use equivalent model capability, reasoning setting,
timeouts, and provider route. The example uses direct DeepSeek V4 Pro with
medium reasoning, the historical canonical configuration.

## Run

Promptfoo needs Node.js and `npx`. The wrapper pins Promptfoo to `0.121.19`,
disables telemetry and cache, and runs one task at a time:

```sh
benchmarks/polyglot/promptfoo/run.sh \
  --config benchmarks/polyglot/promptfoo/config.local.yaml
```

A smoke run may pass normal Promptfoo arguments after the config:

```sh
benchmarks/polyglot/promptfoo/run.sh \
  --config benchmarks/polyglot/promptfoo/config.local.yaml \
  --filter-first-n 1 --repeat 1
```

Each invocation creates a timestamped directory under
`benchmarks/results/promptfoo/` by default. It includes `report.html`,
`results.json`, and viewer state. Detailed agent and validator artifacts remain
at the private `output_root` specified in the config.

Validate adapter-only code without model calls:

```sh
python3 -m unittest benchmarks/polyglot/promptfoo/test_adapter.py
```
