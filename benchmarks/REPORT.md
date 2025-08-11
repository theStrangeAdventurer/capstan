# Capstan vs OpenCode

## Latest comparison

The latest full run used the fixed Aider Polyglot `mini-v2` suite: 12 tasks
across C++, Go, Java, JavaScript, Python, and Rust, repeated three times per
agent. Capstan and OpenCode ran sequentially against the same OpenRouter route,
`deepseek/deepseek-v4-pro`, `medium` reasoning, public task prompts, upstream
tests, and a 240-second per-task timeout.

| Metric | Capstan | OpenCode |
|---|---:|---:|
| Upstream tests passed | 32/36 (88.9%) | 33/36 (91.7%) |
| Median agent wall time | **83.2s** | 83.4s |
| p95 agent wall time | 208.4s | **201.1s** |
| Median local CPU time | **1.38s** | 20.96s |
| Median inclusive peak RSS | **123.8 MiB** | 1044.6 MiB |

The practical result is parity on this workload: one attempt separates the
quality scores and 0.2 seconds separates median wall time. Local overhead is not
close: Capstan used about **15x less local CPU** and **8.4x less inclusive peak
RSS**.

These figures describe this workload, machine, provider route, and model. They
do not establish a universal quality ranking.

## What “inclusive peak RSS” means

The harness starts each agent as a separate process and collects resource usage
with macOS `wait4()`. On macOS, the reported high-water RSS can include child
processes already completed by the agent, such as CMake, clang, Go, Java, npm,
or test runners. It is therefore an end-to-end invocation metric, not the idle
memory of the Capstan executable alone.

The scorer runs afterward in a separate tree, so validator resource usage is
not included. Idle/startup footprint is measured separately by the footprint
harness.

## Reproducibility

Revision identifiers pin inputs that can change over time. A bare hash is not
useful to readers, so published revisions are linked to their source:

- Aider Polyglot corpus:
  [`7e0611e`](https://github.com/Aider-AI/polyglot-benchmark/commit/7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f)
- Capstan code produced from the measured worktree:
  [`4c47c0e`](https://github.com/theStrangeAdventurer/capstan/commit/4c47c0e51e68138911ceb73ac0ef49a5f9ea072b)
- OpenCode:
  [`1.18.15`](https://github.com/anomalyco/opencode/releases/tag/v1.18.15)
- Run date: 2026-08-09
- Platform: Apple Silicon macOS
- Attempts: 72 total, sequential AB/BA ordering

The Capstan run used the pre-release worktree containing the prompt/runtime
changes that were subsequently committed to `main`; it was not a clean tagged
release. The next publication benchmark should be run from a clean release tag.

Raw attempt data, including status, wall time, CPU, and RSS, is committed as
[`polyglot-openrouter-medium-20260809.csv`](polyglot-openrouter-medium-20260809.csv).
The reproducible harness and setup instructions are in
[`polyglot/`](polyglot/README.md).

## Historical results

Earlier direct-DeepSeek quality and standalone footprint datasets remain under
[`historical/`](historical/README.md), with compact CSV files in this directory.
They are retained for auditability but are not presented as current product
figures.
