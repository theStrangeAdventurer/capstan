# Benchmarks

## Purpose

Capstan maintains reproducible, workload-specific comparisons rather than a
single universal performance claim. Benchmark tooling is available to every
repository clone under `benchmarks/`; it is not part of ordinary build or test
targets because it requires external toolchains, network access, and model
credentials.

## Aider Polyglot quality benchmark

`benchmarks/polyglot/` owns the canonical `mini-v2` Aider Polyglot evaluation:

- `bootstrap.sh` clones `Aider-AI/polyglot-benchmark` under
  `benchmarks/work/` and detaches it at the pinned corpus commit;
- `scripts/run_eval.py` owns public-prompt construction, isolated agent and
  scoring worktrees, upstream-test execution, timeouts, result classification,
  and agent-process resource measurement;
- `scripts/run_opencode.py` adapts OpenCode while isolating extension surfaces.
  For OpenRouter it requires `--reasoning-effort` to make a temporary,
  per-model `reasoning.effort` variant; a bare OpenCode `--variant` may be
  absent from the current model catalog and silently omit the setting;
- `.agents/skills/aider-polyglot-evals/SKILL.md` tells any Capstan agent using a
  local clone when and how to run the fixed suite;
- `promptfoo/` is an optional HTML-report layer and must not alter scoring.

The corpus and outputs are intentionally ignored. The corpus contains the
upstream test data; copying it into Capstan would make updates and checkout
integrity harder to manage. Large run logs and provider-specific local paths
must not be committed.

Comparisons must preserve corpus revision, task list, public prompt,
agent/test timeouts, model capability, reasoning setting, provider route, and
sequential execution. A changed value is a new named benchmark configuration.
Each published result records both clean agent revisions, versions, machine
information, provider/model settings, repetition count, raw result data, and
limitations.

## Runtime-footprint benchmark

`benchmarks/footprint/agent-bench.sh` measures executable size, `--help`
startup, and repeated prompt execution. Its prompts have no independent
correctness oracle, so the benchmark is evidence about local client overhead,
not coding quality. It must use the same provider/model/credentials, workspace,
prompt set, and network conditions for compared agents. Alternating order is
the default to reduce warm-cache and rate-limit bias.

## Security and isolation

Benchmark runs use disposable task copies and should use disposable workspaces.
Credentials are supplied through the environment or the user's normal agent
authentication, never benchmark files. Filled Promptfoo config files remain
local because they can expose local paths. The `--benchmark` Capstan mode is
used for agent runs to exclude project instructions, user skills, hooks, MCP,
Wiki, and user plugins from evaluations.

## Validation

- `python3 -m unittest benchmarks/polyglot/promptfoo/test_adapter.py` validates
  the optional report adapter without model calls.
- `./benchmarks/polyglot/bootstrap.sh --check` validates a prepared corpus and
  required toolchains.
- `--dry-run` on `run_eval.py` verifies the task matrix and command before
  provider calls.
- Shell scripts are checked with `bash -n`.
