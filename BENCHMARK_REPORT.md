# Capstan vs opencode: 5-task CLI agent benchmark

Date: 2026-06-29

This report compares Capstan and opencode on a small, repeatable CLI-agent
benchmark. It is intended to be readable enough to attach to the repository or
share publicly, while still keeping the raw methodology visible.

## Summary

Both agents passed every task in the benchmark.

| Agent | Runs | Passes | Median wall-clock | Median CPU | Median peak RSS | Binary size |
|---|---:|---:|---:|---:|---:|---:|
| Capstan | 15 | 15 | 13.797s | 0.810s | 14.4 MiB | 0.89 MiB |
| opencode | 15 | 15 | 30.338s | 6.004s | 511.3 MiB | 113.80 MiB |

On this benchmark, Capstan passed the same tasks while using substantially less
local CPU time, memory, and disk space. Capstan also had the lower median
wall-clock time on all five tasks.

## What was measured

The benchmark measures terminal-agent behavior on five tasks:

1. Recovering when asked to inspect a directory.
2. Editing only the intended file in a dirty git worktree.
3. Treating malformed provider/tool-call noise as inert text.
4. Implementing a Python word-problem parser.
5. Implementing a Python two-bucket solver.

Each task ran three times per agent, in alternating order. Every run used a
fresh workspace.

The metrics are:

- Wall-clock: real elapsed time from command start to command exit.
- CPU: total user + system CPU time consumed by the process tree, as measured by
  the benchmark runner.
- Peak RSS: sampled peak resident memory of the process tree.
- Binary size: filesystem size of the agent executable used in the run.

Wall-clock includes model/provider latency. CPU and RSS are local machine
resource measurements.

## Environment

| Item | Value |
|---|---|
| OS | macOS 26.5.1, Darwin 25.5.0 |
| Architecture | arm64 |
| CPU | Apple M5 |
| RAM | 32 GiB |
| Compiler | Apple clang 21.0.0 |
| Model/provider | OpenRouter, `deepseek/deepseek-v4-pro` |
| Capstan binary | `/Users/alxd/.local/bin/capstan` |
| opencode binary | `/Users/alxd/.opencode/bin/opencode` |
| opencode version | 1.17.3 |

## Commands

The default benchmark entry point is:

```sh
~/Benchmarks/run-agent-evals-5.sh
```

For this report it was run as:

```sh
RUNS=3 TIMEOUT=240 ~/Benchmarks/run-agent-evals-5.sh
```

The task file was:

```text
~/Benchmarks/agent-evals/tasks/benchmark-5.jsonl
```

The Capstan command template was:

```sh
{bin} run --benchmark \
  --provider openrouter \
  --model deepseek/deepseek-v4-pro \
  --prompt-file {prompt_file} \
  --workdir {workdir}
```

The opencode command template was:

```sh
python3 /Users/alxd/Benchmarks/opencode_prompt_file.py \
  --bin {bin} \
  --model openrouter/deepseek/deepseek-v4-pro \
  --workdir {workdir} \
  --prompt-file {prompt_file} \
  --dangerously-skip-permissions
```

## Task results

The table below reports median values across three runs per agent.

| Task | Runs passed | Capstan wall-clock | opencode wall-clock | Difference | Winner | Capstan CPU | opencode CPU | Capstan RSS | opencode RSS |
|---|---:|---:|---:|---:|---|---:|---:|---:|---:|
| `directory-read-recovery` | 3/3 vs 3/3 | 13.797s | 21.644s | -7.847s | Capstan | 0.810s | 5.000s | 14.4 MiB | 484.3 MiB |
| `dirty-worktree-scope` | 3/3 vs 3/3 | 12.427s | 16.482s | -4.056s | Capstan | 0.672s | 4.894s | 14.2 MiB | 509.0 MiB |
| `malformed-tool-tag-noise` | 3/3 vs 3/3 | 4.840s | 7.604s | -2.764s | Capstan | 0.301s | 2.316s | 13.9 MiB | 463.0 MiB |
| `polyglot-python-two-bucket` | 3/3 vs 3/3 | 116.402s | 117.374s | -0.972s | Capstan | 6.266s | 29.729s | 27.0 MiB | 578.6 MiB |
| `polyglot-python-wordy` | 3/3 vs 3/3 | 151.443s | 171.288s | -19.844s | Capstan | 8.007s | 43.833s | 22.0 MiB | 577.8 MiB |

`Difference` is `Capstan wall-clock - opencode wall-clock`; negative means
Capstan was faster.

## Binary size

| Agent | Binary path | Bytes | Size |
|---|---|---:|---:|
| Capstan | `/Users/alxd/.local/bin/capstan` | 934,520 | 0.89 MiB |
| opencode | `/Users/alxd/.opencode/bin/opencode` | 119,322,722 | 113.80 MiB |

Capstan is a C/Lua binary with vendored Lua and ncurses linked statically.
opencode is distributed as a much larger standalone binary.

## Raw artifacts

The raw benchmark output for this report is stored at:

```text
~/Benchmarks/agent-evals-results/20260629-095330/
```

Important files:

- `summary.md`: runner-generated summary.
- `results.csv`: raw per-run measurements.
- `runs/<agent>.<task>.runN/`: prompt, command, stdout, stderr, setup logs, and
  final workspace for each run.

A copy of the report artifacts is stored at:

```text
~/Benchmarks/final/
```

## Limitations

This benchmark is intentionally small. It is not a universal claim about every
coding workload or every model. The results depend on the chosen model,
provider latency, local machine, task mix, and command templates.

The benchmark is still useful because it is concrete and reproducible: the task
definitions, prompts, command templates, raw stdout/stderr, final workspaces,
and CSV measurements are all preserved.

For broader claims, run the extended suite:

```sh
~/Benchmarks/run-agent-evals-extended.sh
```

The default report uses five tasks because the extended suite is much slower and
is better suited for periodic validation rather than every iteration.
