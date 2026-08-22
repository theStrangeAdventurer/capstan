# Capstan vs OpenCode

## Latest local comparison

The latest run used the fixed Aider Polyglot `mini-v2` suite: 12 tasks across
C++, Go, Java, JavaScript, Python, and Rust, repeated three times per agent.
Capstan and OpenCode ran sequentially against direct DeepSeek V4 Pro with
`medium` reasoning, public task prompts, upstream tests, and a 240-second
per-task timeout.

| Metric | Capstan | OpenCode |
|---|---:|---:|
| Upstream tests passed | 35/36 (97.2%) | **36/36 (100%)** |
| Median agent wall time | 48.7s | **46.3s** |
| p95 agent wall time | 158.7s | **117.9s** |
| Median local CPU time | **1.64s** | 16.44s |
| Median primary-PID peak RSS | **18.6 MiB** | 1073.2 MiB |
| Highest primary-PID peak RSS | **29.8 MiB** | 1163.9 MiB |

Capstan had one `agent_timeout` on `javascript/promises`; all other attempts
passed upstream tests. On this workload OpenCode was slightly more reliable and
faster, while Capstan used about 10x less median local CPU and 58x less median
primary-process RSS.

These figures are workload-, environment-, route-, and model-specific. They do
not establish a universal quality ranking.

## What “primary-PID peak RSS” means

The harness samples the main agent PID every 50 ms. This excludes child test
runners and tool processes such as CMake, clang, Go, Java, npm, and Python. For
OpenCode, the adapter reports the OpenCode process rather than its Python
wrapper. Sampling can miss peaks shorter than 50 ms.

CPU remains the `wait4()` usage for the complete agent invocation. The raw
`wait4()` high-water RSS is also retained in the CSV for workload diagnostics,
but it is not used for the primary memory comparison. The scorer runs later in
a separate tree and is measured separately.

## Reproducibility

- Aider Polyglot corpus:
  [`7e0611e`](https://github.com/Aider-AI/polyglot-benchmark/commit/7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f), clean
- Capstan base revision:
  [`df0f3f5`](https://github.com/theStrangeAdventurer/capstan/commit/df0f3f5dbea660d110c8355ddabe67988bcf62c0)
- OpenCode:
  [`1.18.15`](https://github.com/anomalyco/opencode/releases/tag/v1.18.15)
- Provider/model: direct DeepSeek, `deepseek-v4-pro`
- Reasoning: `medium`
- Run date: 2026-08-21
- Platform: Apple M5, macOS 26.5.1, 32 GiB RAM
- Attempts: 72 total, six complete sequential suites

The Capstan worktree included all current prompt, runtime, benchmark, and other
changes under evaluation. Runs 1 and 2 completed before the provider balance
was exhausted; both agents' incomplete run 3 was discarded and each complete
run 3 was repeated after service was restored.

Raw attempt data is stored as
[`polyglot-deepseek-medium-primary-rss-20260821.csv`](polyglot-deepseek-medium-primary-rss-20260821.csv).
The reproducible harness and setup instructions are in
[`polyglot/`](polyglot/README.md).

## Historical results

The 2026-08-09 OpenRouter dataset is retained as
[`polyglot-openrouter-medium-20260809.csv`](polyglot-openrouter-medium-20260809.csv).
Its RSS field came from `wait4()` and can include completed child processes, so
it must not be compared directly with the primary-PID RSS above. Earlier
quality and standalone footprint datasets remain under
[`historical/`](historical/README.md). Historical files are kept for
auditability rather than deleted when measurement methodology changes.
