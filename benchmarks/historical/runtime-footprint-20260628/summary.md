# Agent Benchmark Summary

Order mode: `alternating`

## Static

| Agent | Binary | Exists | Size | Self-test exit | Self-test elapsed |
|---|---:|---:|---:|---:|---:|
| capstan | `<home>/.local/bin/capstan` | 1 | 896K | 0 | 2.453s |
| opencode | `<home>/.opencode/bin/opencode` | 1 | 114M |  | s |

## Task Runtime

| Agent | Task | Runs | Failures | Mean | Median | Min | Max | Stddev |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| capstan | 01-bug-find | 5 | 0 | 29.197s | 18.326s | 12.953s | 68.231s | 22.888s |
| capstan | 02-async-rewrite | 5 | 0 | 13.837s | 12.730s | 10.116s | 20.165s | 3.851s |
| capstan | 03-commit-message | 5 | 0 | 5.898s | 5.401s | 4.509s | 7.378s | 1.348s |
| opencode | 01-bug-find | 5 | 0 | 17.358s | 16.031s | 12.228s | 21.553s | 3.972s |
| opencode | 02-async-rewrite | 5 | 0 | 7.439s | 5.388s | 4.695s | 15.417s | 4.529s |
| opencode | 03-commit-message | 5 | 0 | 7.172s | 7.447s | 5.965s | 7.644s | 0.684s |

## Median Delta

Delta is `capstan` relative to `opencode`. Negative means faster/smaller.

| Task | capstan median | opencode median | Delta |
|---|---:|---:|---:|
| 01-bug-find | 18.326s | 16.031s | +14.3% |
| 02-async-rewrite | 12.730s | 5.388s | +136.3% |
| 03-commit-message | 5.401s | 7.447s | -27.5% |

## Caveats

- Use the same provider, model, API keys, and network conditions for both agents.
- The agents have different built-in system prompts; treat that as a product-level caveat.
- Context RAM is not generic unless both agents expose the same history import/export API.
- Raw stdout/stderr and time logs are in `raw/`; machine-readable data is in `*.csv`.
