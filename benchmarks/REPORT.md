# Capstan vs OpenCode

## Latest local comparison — OpenRouter throughput, 2026-10-10

The latest exploratory comparison ran 12 Aider Polyglot `mini-v2` tasks three
times per agent with DeepSeek V4 Pro through OpenRouter, medium reasoning,
explicit throughput routing for both agents, and a 240-second timeout.
Six suites ran sequentially in seeded, balanced agent order.

| Metric (36 attempts per agent) | Capstan | OpenCode |
|---|---:|---:|
| Upstream tests passed | **36/36 (100%)** | **36/36 (100%)** |
| Mean agent wall time | 33.67s | 33.77s |
| Median agent wall time | 24.21s | 26.37s |
| Total agent CPU time | **107.3s** | 520.9s |
| Mean primary-PID peak RSS | **33.4 MiB** | 1130.4 MiB |
| Maximum primary-PID peak RSS | **66.9 MiB** | 1225.4 MiB |

No errors or timeouts. Wall time was effectively equal; Capstan used about
4.86x less aggregate CPU and 33.87x less mean primary-PID peak RSS.

Compared with the previous full local OpenRouter series (2026-10-09), Capstan's
mean agent wall time fell from 119.04s to 33.67s (3.54x, 71.7% reduction),
and its score rose from 34/36 to 36/36. This followed the lossless SSE fix:
packets containing `usage` alongside text/tool deltas previously lost fields;
they now dispatch all supported fields. **The acceleration is not attributable
to SSE alone:** routing changed too, and OpenCode also accelerated (65.93s to
33.77s). This is not a controlled single-change experiment.

Full methodology, provenance, telemetry, baseline and limitations:
[`historical/openrouter-throughput-20261010/`](historical/openrouter-throughput-20261010/README.md).
Numeric data: [`polyglot-openrouter-throughput-20261010.csv`](polyglot-openrouter-throughput-20261010.csv).
The measured worktree was dirty; the implementation was subsequently committed
as `fcfd3cb0`. Committing afterwards does not turn this into a clean-release run.
Historical results below are retained unchanged.

## Previous local comparison — direct DeepSeek, 2026-08-21

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
