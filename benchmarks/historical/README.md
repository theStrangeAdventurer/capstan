# Historical benchmark artifacts

This directory preserves the machine-readable evidence behind the public
2026-06-28 runtime-footprint and 2026-07-31 Aider Polyglot reports.

- `polyglot-mini-v2-20260731/results.json` is the full Promptfoo result export
  for all 72 quality attempts. The compact CSV one directory up is the
  publication-oriented table derived from it.
- `runtime-footprint-20260628/` contains the runner's `static.csv`, `runs.csv`,
  `idle.csv`, `config.txt`, and `summary.md`. The compact CSV one directory up
  contains the 30 per-run CPU and RSS observations used in the public report.
- `polyglot-direct-prompt-20260829/` contains a compact 72-attempt CSV and trace
  analysis for an exploratory direct-DeepSeek comparison after prompt and
  completion-review changes. Its dirty Capstan worktree makes it research
  evidence rather than a publishable release result.

Absolute home and temporary paths have been replaced with `<home>` and `<tmp>`.
Promptfoo's cache/database and per-run stdout/stderr logs are excluded: they are
not required to verify the published aggregates and can contain machine-local
paths or model output.

These are historical results, not current Capstan measurements. New runs use
the checked-in harnesses in `../polyglot/` and `../footprint/` and write their
complete artifacts to ignored output directories.
