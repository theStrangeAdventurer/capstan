# Benchmark: Capstan vs OpenCode — DeepSeek V4 Pro (OpenRouter)

**Date:** 2025-09-30
**Comparison ID:** `deepseek-v4-pro-medium-openrouter`
**Corpus:** [Aider-AI/polyglot-benchmark](https://github.com/Aider-AI/polyglot-benchmark) @ `7e0611e`
**Suite:** `mini-v2` (12 tasks, 6 languages)
**Timeout:** 240s agent, 300s tests
**Repetitions:** 2 (interleaved: Capstan r1 → OpenCode r1 → Capstan r2 → OpenCode r2)

## Score

| Run | Capstan | OpenCode |
|-----|---------|----------|
| **r1** | **12/12** | **12/12** |
| **r2** | **12/12** | **11/12** |
| **Total** | **24/24 (100%)** | **23/24 (95.8%)** |

OpenCode's only failure: `python/pov` hit the 240s agent timeout in r2.

## Aggregate wall time

| Metric | Capstan | OpenCode | Diff |
|--------|---------|----------|------|
| Total agent wall time | 1582.1s | 2047.9s | Capstan **22.7% faster** |
| Median task wall time | 46.7s | 55.2s | Capstan **15.4% faster** |
| p95 task wall time | 132.7s | 237.5s | Capstan **44.1% faster** |

Note: total wall includes the failed `python/pov` run at exactly 240.1s for OpenCode.

## Per-run wall time

| Run | Capstan | OpenCode |
|-----|---------|----------|
| r1 | 913.0s | 873.8s |
| r2 | 669.1s | 1174.1s |

OpenCode r2 is inflated by the 240s timeout; Capstan r2 shows the fastest suite time of all four runs.

## Resource usage

| Metric | Capstan (total) | OpenCode (total) | Ratio |
|--------|-----------------|-------------------|-------|
| CPU time (user+sys) | **61.6s** | 286.4s | **4.6× lower** |
| Peak main-process RSS | **59.0 MiB** | 1177.3 MiB | **~20× smaller** |

The harness samples only the primary agent PID every 50 ms, excluding child
compilers, test runners, and tool processes.

## Per-task breakdown

| Task | Cap r1 | Cap r2 | OC r1 | OC r2 |
|------|--------|--------|-------|-------|
| `cpp/clock` | 71.4s | 39.4s | 70.5s | 46.1s |
| `cpp/grade-school` | 50.3s | 39.1s | 44.1s | 61.5s |
| `go/protein-translation` | 24.0s | 28.3s | 17.9s | 21.2s |
| `go/transpose` | 129.4s | 113.7s | 113.0s | 237.5s |
| `java/phone-number` | 46.7s | 46.7s | 109.1s | 115.8s |
| `java/series` | 33.4s | 22.5s | 40.7s | 26.1s |
| `javascript/promises` | 104.6s | 120.7s | 173.3s | 149.5s |
| `javascript/triangle` | 80.9s | 28.2s | 32.6s | 57.5s |
| `python/pov` | 190.4s | 132.7s | 97.2s | **240.1s timeout** |
| `python/forth` | 87.1s | 46.2s | 109.4s | 135.8s |
| `rust/acronym` | 68.1s | 20.6s | 43.7s | 30.1s |
| `rust/word-count` | 26.8s | 31.0s | 22.3s | 52.8s |

All 47 completed tasks passed upstream tests.

## Per-task CPU and RSS

### Capstan

| Task | r1 CPU | r1 RSS | r2 CPU | r2 RSS |
|------|--------|--------|--------|--------|
| `cpp/clock` | 2.67s | 22.9 MiB | 2.64s | 21.3 MiB |
| `cpp/grade-school` | 2.71s | 23.9 MiB | 2.65s | 23.4 MiB |
| `go/protein-translation` | 0.56s | 21.4 MiB | 0.57s | 22.0 MiB |
| `go/transpose` | 1.14s | 50.5 MiB | 1.09s | 47.1 MiB |
| `java/phone-number` | 0.84s | 25.6 MiB | 0.84s | 22.9 MiB |
| `java/series` | 0.75s | 22.1 MiB | 0.72s | 22.5 MiB |
| `javascript/promises` | 10.39s | 29.3 MiB | 12.83s | 41.6 MiB |
| `javascript/triangle` | 7.32s | 25.2 MiB | 7.20s | 24.3 MiB |
| `python/pov` | 1.13s | 59.0 MiB | 0.84s | 42.9 MiB |
| `python/forth` | 0.61s | 29.7 MiB | 0.56s | 31.0 MiB |
| `rust/acronym` | 1.00s | 25.8 MiB | 0.81s | 24.0 MiB |
| `rust/word-count` | 0.84s | 21.3 MiB | 0.84s | 34.4 MiB |

### OpenCode

| Task | r1 CPU | r1 RSS | r2 CPU | r2 RSS |
|------|--------|--------|--------|--------|
| `cpp/clock` | 13.50s | 1024.0 MiB | 9.71s | 1177.3 MiB |
| `cpp/grade-school` | 9.86s | 1086.2 MiB | 10.77s | 1064.9 MiB |
| `go/protein-translation` | 5.34s | 1026.9 MiB | 5.60s | 1026.3 MiB |
| `go/transpose` | 14.66s | 1040.4 MiB | 27.58s | 1097.9 MiB |
| `java/phone-number` | 13.01s | 1042.8 MiB | 13.74s | 1056.8 MiB |
| `java/series` | 7.19s | 1054.2 MiB | 6.13s | 1119.9 MiB |
| `javascript/promises` | 26.87s | 1033.8 MiB | 14.20s | 1046.9 MiB |
| `javascript/triangle` | 13.20s | 1017.7 MiB | 16.23s | 1107.3 MiB |
| `python/pov` | 13.39s | 1053.5 MiB | 0.03s | 1014.4 MiB |
| `python/forth` | 15.37s | 1078.4 MiB | 16.16s | 1091.8 MiB |
| `rust/acronym` | 9.88s | 1049.1 MiB | 8.52s | 1058.7 MiB |
| `rust/word-count` | 6.75s | 1088.7 MiB | 8.74s | 1046.1 MiB |

## Configuration

| Parameter | Capstan | OpenCode |
|-----------|---------|----------|
| Command | `capstan run --benchmark --no-wiki --provider openrouter --model deepseek/deepseek-v4-pro --profile implement --reasoning-effort medium --max-turns 40 --prompt-file {prompt_file} --workdir {workdir} --workspace {workdir} --json --trace-file {trace_file}` | `python3 run_opencode.py --prompt-file {prompt_file} --workdir {workdir} --model openrouter/deepseek/deepseek-v4-pro --reasoning-effort medium` |
| Model | `deepseek/deepseek-v4-pro` | `openrouter/deepseek/deepseek-v4-pro` |
| Reasoning | `medium` | `medium` |
| Agent timeout | 240s | 240s |
| Test timeout | 300s | 300s |
| Prompts | Public upstream prompt per task | Same public upstream prompt |
| Tests | Upstream test suite per task | Same upstream test suite |

Both agents used the same OpenRouter API key, same model, and same
reasoning-effort setting. Prompts, test suites, and timeouts were identical.

## Architecture comparison

| Attribute | Capstan | OpenCode |
|-----------|---------|----------|
| Language | C (core) + Lua (policy) | TypeScript (Node.js) |
| Binary | Single static Mach-O / ELF | npm package + Node runtime |
| UI | Native ncurses | Terminal-kit / Ink |
| Dependencies | libcurl (dynamic), ncurses+Lua (static) | npm dependency tree |

## Raw data

```
benchmarks/historical/polyglot-openrouter-20260828/
├── capstan-polyglot-r1/
│   ├── metadata.json
│   ├── results.json
│   └── summary.md
├── capstan-polyglot-r2/
│   ├── metadata.json
│   ├── results.json
│   └── summary.md
├── opencode-polyglot-r1/
│   ├── metadata.json
│   ├── results.json
│   └── summary.md
└── opencode-polyglot-r2/
    ├── metadata.json
    ├── results.json
    └── summary.md
```

Task directories with full traces are available in the (larger) original
results at `benchmarks/polyglot/results/deepseek-v4-pro-medium-openrouter/`.