# OpenRouter throughput comparison — 2026-10-10

## Results

Aider Polyglot `mini-v2`: 12 unchanged public tasks, three complete repetitions
per agent, 72 attempts scored using upstream tests in separate scoring trees.

| Metric (36 attempts per agent) | Capstan | OpenCode |
|---|---:|---:|
| Upstream tests passed | 36/36 (100%) | 36/36 (100%) |
| Time including scoring | 1271.363s | 1273.701s |
| Total agent wall time | 1212.220s | 1215.774s |
| Mean agent wall time | 33.67s | 33.77s |
| Median agent wall time | 24.21s | 26.37s |
| Total agent CPU time | 107.3s | 520.9s |
| Mean primary-PID peak RSS | 33.4 MiB | 1130.4 MiB |
| Maximum primary-PID peak RSS | 66.9 MiB | 1225.4 MiB |

No agent timeouts, agent errors, test failures, or harness errors. Agent wall
speed is effectively equal; Capstan used 4.86x less aggregate CPU
and 33.87x less mean primary-PID peak RSS.
RSS is sampled every 50 ms for the primary agent only, excluding tool/test
children. CPU is `wait4` usage for the invocation and may include children;
scoring is measured separately. Do not interpret RSS as whole-process-tree RAM.

## Configuration and provenance

- Corpus: `7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f`.
- Provider/model: OpenRouter, `deepseek/deepseek-v4-pro`.
- Reasoning: `medium`; routing: explicit `provider.sort = throughput` for both.
- Agent timeout: 240s; validator timeout: 300s; Capstan max turns: 40.
- OpenCode version: `1.18.33`; adapter uses its `build` agent.
- Machine: Apple M5, 32 GiB RAM, `macOS-26.5.1-arm64-arm-64bit-Mach-O`.
- Random seed: `1472251079`; sequential order:
  OpenCode r1 → Capstan r1 → Capstan r2 → OpenCode r2 → Capstan r3 → OpenCode r3.
- Capstan base at execution: `2205ca8112951059636abbe1b19d12a66e9f230a` plus dirty changes.
- Implementation subsequently recorded as `fcfd3cb023f4fa5f0de720b9ebf0c1a3ffd0ec7e`. This is not a claim
  that the run started on that clean commit. The captured worktree status,
  measured binary SHA-256, exact commands, harness/config hashes and source
  checksums are in [manifest.json](manifest.json).
- Raw local logs, traces, task workspaces and results remain at
  `benchmarks/work/paired-openrouter-20261010-004745` (Git-ignored). Auth logs, credentials and large workspaces are not
  exported. Compact numeric attempt tables and summaries are committed.

## Acceleration relative to the previous full OpenRouter series

Previous source: `benchmarks/work/comparison-20261009-174510` (2026-10-09), same corpus/model,
medium reasoning and 240s timeout, but automatic routing and earlier runtime.
The [previous attempt table](previous-auto-routing-attempts.csv) retains every
attempt, including timeouts; do not mix these configurations into one sample.

| Agent | Previous mean agent wall | Current mean agent wall | Ratio old/new |
|---|---:|---:|---:|
| Capstan | 119.04s | 33.67s | 3.54x |
| OpenCode | 65.93s | 33.77s | 1.95x |

Capstan went from 34/36 passed (two timeouts) to 36/36; OpenCode from
35/36 (one timeout) to 36/36. Capstan mean wall time fell by
71.7% after the SSE/routing changes.

**SSE fix:** a packet containing `usage` together with text or a tool call was
previously processed only partially due to early returns. The parser now
preserves and dispatches all supported semantic fields: reasoning, text,
tool-call deltas and usage, including fragmented/trailing packets. Regression
tests independently demonstrate the correctness bug and its fix.

**Causality limit:** this experiment does not isolate the SSE fix. OpenRouter
routing also changed to throughput preference for both agents, and provider
load/backends and stochastic solutions can differ. The observed 3.54x Capstan
acceleration is an after-change comparison, not proof that SSE alone caused
it. OpenCode also accelerated. A same-route before/after or factorial experiment
would be needed to separate effects. Throughput routing can increase cost;
no cost comparison was measured.

## Telemetry

Capstan complete, process-consistent traces and breakdown metrics: 36/36;
partial, corrupt or inconsistent traces: 0. Breakdown averages and model-request/
tool-call counts per task are preserved in [analysis.tsv](analysis.tsv);
per-attempt runtime measurements are in the
[72-attempt CSV](../../polyglot-openrouter-throughput-20261010.csv).
Model, tool, permission, subagent and unattributed times are producer measurements,
not inferred from log timestamps. Overlap measurement is unavailable (N/A),
not zero. OpenCode raw event logs remain in the local source directory; no
comparable runtime breakdown is claimed for OpenCode.

This is an exploratory local comparison, not a clean-release reproducibility
claim or universal ranking. Results are specific to these tasks, settings,
machine and service conditions.
