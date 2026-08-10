# Capstan vs OpenCode: quality, speed, and local footprint

## Result

The canonical 12-task Aider Polyglot `mini-v2` suite was run three times for
each agent on 2026-07-31. Both agents used direct DeepSeek V4 Pro with medium
reasoning, sequential execution, the same public prompts and upstream tests,
and a 240-second agent timeout.

| Agent | Canonical passes | Pass rate | Median agent time | Total agent time |
|---|---:|---:|---:|---:|
| **Capstan** | **36/36** | **100%** | **64.3s** | **53m 19.0s** |
| OpenCode | 35/36 | 97.2% | 79.0s | 58m 09.2s |

Capstan completed every attempt and led by one canonical pass. Its median
agent time was 18.7% lower, and its 36 agent runs took 4m 50.1s less in total.
The only failure was OpenCode run 3 on `python/forth`, which reached the
240-second timeout. There were no HTTP 400, empty-response, agent-error,
test-failure, or harness-error results.

Open the [standalone HTML report](BENCHMARK_REPORT.html) for the visual summary.

## All 72 attempts

Times below are agent wall time; validator time is excluded. Each cell contains
the canonical pass count, median, then runs 1–3.

| Task | Capstan: pass · median · runs | OpenCode: pass · median · runs |
|---|---|---|
| `cpp/clock` | 3/3 · 46.5s · 53.8, 39.6, 46.5 | 3/3 · 52.2s · 49.4, 77.1, 52.2 |
| `cpp/grade-school` | 3/3 · 93.8s · 93.8, 123.9, 79.9 | 3/3 · 68.8s · 66.7, 68.8, 89.3 |
| `go/protein-translation` | 3/3 · 27.6s · 55.7, 26.9, 27.6 | 3/3 · 35.9s · 39.0, 34.0, 35.9 |
| `go/transpose` | 3/3 · 170.2s · 151.6, 170.2, 173.6 | 3/3 · 159.0s · 134.3, 169.3, 159.0 |
| `java/phone-number` | 3/3 · 59.3s · 59.3, 126.5, 58.0 | 3/3 · 75.0s · 58.9, 75.0, 209.2 |
| `java/series` | 3/3 · 50.3s · 50.3, 55.5, 33.3 | 3/3 · 32.6s · 31.7, 34.2, 32.6 |
| `javascript/promises` | 3/3 · 151.5s · 151.5, 64.6, 167.1 | 3/3 · 123.9s · 88.3, 189.9, 123.9 |
| `javascript/triangle` | 3/3 · 46.5s · 46.5, 74.3, 38.6 | 3/3 · 50.5s · 50.5, 35.8, 71.5 |
| `python/pov` | 3/3 · 122.7s · 147.3, 106.1, 122.7 | 3/3 · 129.2s · 167.8, 117.1, 129.2 |
| `python/forth` | 3/3 · 119.4s · 111.4, 119.4, 143.7 | 2/3 · 124.5s · 124.0, 124.5, **timeout 240.0** |
| `rust/acronym` | 3/3 · 54.5s · 64.2, 54.5, 53.3 | 3/3 · 90.8s · 90.8, 71.6, 105.8 |
| `rust/word-count` | 3/3 · 52.2s · 54.0, 52.2, 45.8 | 3/3 · 70.2s · 70.2, 49.9, 100.4 |

## Local runtime footprint

CPU and memory were not recorded during the July Polyglot run and cannot be
reconstructed afterward. The figures below come from the separate controlled
runtime benchmark performed on the same Apple Silicon Mac on 2026-06-28. It
ran three neutral tasks five times per agent, alternated agent order, used the
same OpenRouter `deepseek/deepseek-v4-pro` model for both agents, and measured
each command with macOS `/usr/bin/time -l`.

| Metric, median of 15 runs | Capstan | OpenCode | Capstan difference |
|---|---:|---:|---:|
| Peak resident memory | **14.7 MiB** | 555.0 MiB | **37.8x lower** |
| Local CPU time (`user + sys`) | **0.19s** | 2.97s | **15.6x lower** |
| Measured executable size | **896 KiB** | 114 MiB | **about 130x smaller** |

Peak RSS ranged from 14.1–23.9 MiB for Capstan and 529.9–655.8 MiB for
OpenCode. Mean peak RSS was 15.4 MiB and 570.3 MiB respectively. Network wait
dominates wall time in this small workload, so CPU time is the useful local
overhead measure; it is not a claim about provider compute.

This footprint result is evidence about the local clients, not a second quality
score. The workload, date, and provider route differ from the July Polyglot
comparison, so the two datasets are presented side by side but never merged.

## Configuration and reproducibility

| Item | Polyglot quality run | Runtime footprint run |
|---|---|---|
| Date | 2026-07-31 | 2026-06-28 |
| Machine | Apple Silicon macOS | Same local Apple Silicon Mac |
| Tasks | Aider Polyglot `mini-v2`, 12 fixed tasks | 3 neutral tasks |
| Repetitions | 3 per agent (72 attempts total) | 5 per task and agent (30 attempts total) |
| Provider | Direct DeepSeek | OpenRouter |
| Model | DeepSeek V4 Pro | `deepseek/deepseek-v4-pro` |
| Agent timeout | 240 seconds | no benchmark timeout recorded |
| Execution | sequential, Promptfoo cache disabled | sequential, alternating order |
| Quality scorer | canonical upstream tests | not a quality benchmark |
| Resource measurement | not collected | macOS `/usr/bin/time -l` |
| Corpus commit | `7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f` | n/a |
| Capstan revision | `b7a664ef`, dirty worktree | earlier local build |
| OpenCode | 1.18.2 | local build measured at 114 MiB |

The Promptfoo evaluation ID is `eval-ANr-2026-07-31T10:33:38`.
Machine-readable historical data is committed with this report:

- [`benchmarks/polyglot-mini-v2-20260731.csv`](benchmarks/polyglot-mini-v2-20260731.csv)
  contains all 72 quality attempts;
- [`benchmarks/runtime-footprint-20260628.csv`](benchmarks/runtime-footprint-20260628.csv)
  contains all 30 resource measurements;
- [`benchmarks/historical/`](benchmarks/historical/README.md) contains the
  sanitized full Promptfoo export and runtime-runner metadata. The compact CSV
  files are the preserved numeric source for the published aggregates.

Promptfoo's cache/database and per-run stdout/stderr logs are not committed:
those are not needed to verify the published aggregates and may contain
machine-local paths or model output.

## Limitations

- Thirty-six attempts per agent reveal useful differences but do not establish
  a universal ranking across models, repositories, or machines.
- Provider latency and transient provider behavior are part of wall time.
- Capstan was tested from a dirty worktree, so the commit alone does not fully
  reconstruct the tested binary.
- The single OpenCode timeout materially affects its total time but not the
  reported median.
- CPU/RSS and quality were measured in separate experiments. Future Promptfoo
  runs should collect process resource metrics directly so they can be shown
  per attempt.
