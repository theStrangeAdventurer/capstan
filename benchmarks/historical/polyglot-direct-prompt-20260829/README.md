# Exploratory direct DeepSeek prompt comparison — 2026-08-29

This directory preserves a compact, sanitized export of an exploratory Aider
Polyglot `mini-v2` comparison after changes to Capstan's implement prompt and
completion-review policy. Raw traces and model output remain outside Git.

## Configuration

- Corpus: `7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f`, clean
- Capstan base revision: `ec608ac19ad5ed9446d5dd57f09e27f022ce500e`
- Capstan worktree: dirty; prompt/runtime changes under evaluation were uncommitted
- OpenCode: `1.18.15`
- Provider/model: direct DeepSeek, `deepseek-v4-pro`
- Reasoning: `medium`
- Agent timeout: 240 seconds; test timeout: 300 seconds
- Platform: Apple M5, macOS 26.5.1, arm64
- Runs: three complete sequential suites per agent, balanced as
  `r1 Capstan→OpenCode`, `r2 OpenCode→Capstan`, `r3 Capstan→OpenCode`
- Comparison ID: `deepseek-v4-pro-medium-direct-prompt-20260829-224935`

Because the Capstan worktree was dirty, this is research evidence rather than a
publishable release benchmark.

## Result

| Metric | Capstan | OpenCode |
|---|---:|---:|
| Upstream tests passed | 36/36 | 36/36 |
| Total agent wall time | 2605.24s | **1724.83s** |
| Mean agent wall time | 72.37s | **47.91s** |
| Median agent wall time | 67.45s | **32.02s** |
| p95 agent wall time | 177.10s | **131.51s** |
| Total local CPU time | **118.16s** | 632.79s |
| Median local CPU time | **1.92s** | 13.04s |
| Median primary-PID peak RSS | **20.34 MiB** | 1100.94 MiB |
| Highest primary-PID peak RSS | **29.25 MiB** | 1181.38 MiB |

Capstan used 81.3% less local CPU in aggregate and about 54x less median
primary-process RSS, but took 51.0% more aggregate agent wall time. The earlier
49.2% figure based on total task duration included scorer overhead; agent
process wall time is the authoritative comparison here.

## Trace findings

All 36 Capstan traces were complete and process-consistent:

- 281 model requests and 270 tool calls;
- 2436.64s model time (93.5% of agent wall time);
- 142.06s tool time;
- 2.38s unattributed time;
- no permission or subagent wait.

OpenCode recorded 223 model steps and 285 tool uses. The formats are not
identical, but they show the main structural difference: OpenCode performed
more tool actions in fewer model rounds.

Five tasks account for 80.2% of Capstan's aggregate wall-time deficit:

| Task | Capstan mean | OpenCode mean | Paired delta |
|---|---:|---:|---:|
| `cpp/grade-school` | 102.36s | 37.09s | +65.27s |
| `java/phone-number` | 90.76s | 31.25s | +59.51s |
| `javascript/promises` | 148.88s | 102.25s | +46.63s |
| `python/pov` | 99.85s | 64.46s | +35.39s |
| `python/forth` | 149.48s | 120.81s | +28.67s |

The slow tasks combine extra model rounds with long provider responses. Examples:

- `cpp/grade-school`: 8–10 Capstan requests and 8–14 tool calls per run;
  unexpected `fetch` calls occurred in every repetition.
- `java/phone-number`: 9–11 requests per run, with repeated reads and shell
  checks after implementation.
- `javascript/promises`: 14–15 requests per run and many repeated shell checks.
- `python/forth`: only 6–7 requests, but individual model responses took up to
  123 seconds; this is mostly model/provider latency rather than local runtime.

## Change versus the preceding exploratory run

Compared with the immediately preceding exploratory run (whose raw artifacts
were not committed), the prompt/review changes had a small positive aggregate
effect on Capstan:

- agent wall time: 2695.09s → 2605.24s (−3.3%);
- model requests: 285 → 281;
- tool calls: 281 → 270;
- model time: 2505.13s → 2436.64s;
- tool time: 160.83s → 142.06s;
- CPU time: 128.55s → 118.16s (−8.1%);
- maximum primary-PID RSS: 32.02 MiB → 29.25 MiB.

The improvement was not broad or stable: Capstan median wall time increased
from 61.71s to 67.45s, p95 increased from 165.40s to 177.10s, and mean RSS rose
from 18.94 MiB to 20.11 MiB. OpenCode also became faster in this later provider
window (1927.03s → 1724.83s), so Capstan's relative deficit increased.

The compact per-attempt evidence is in [`attempts.csv`](attempts.csv). Results
are workload-, model-, provider-, and environment-specific and do not establish
a universal agent ranking.
