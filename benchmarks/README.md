# Benchmarks

Capstan keeps benchmark tooling, raw published CSV files, and methodology in
this directory. Generated corpora, task worktrees, logs, reports, and private
provider configuration are ignored by Git.

- [`polyglot/`](polyglot/README.md) — reproducible 12-task Aider Polyglot
  `mini-v2` coding-quality benchmark. Start with
  `./benchmarks/polyglot/bootstrap.sh`.
- [`footprint/`](footprint/README.md) — local executable-size, startup, and
  repeated-prompt overhead benchmark. It is not a correctness benchmark.
- [`polyglot-mini-v2-20260731.csv`](polyglot-mini-v2-20260731.csv) and
  [`runtime-footprint-20260628.csv`](runtime-footprint-20260628.csv) — compact
  raw tables behind the historical public report.
- [`historical/`](historical/README.md) — sanitized full result export and
  runner data for those historical comparisons.

Benchmark runs require external toolchains, a provider credential, and can
consume model time. They are intentionally separate from `make test`. Read the
chosen benchmark's README and run its dry-run or check command before making
provider calls.

When Capstan is started from this clone, it discovers the project skill at
`.agents/skills/aider-polyglot-evals/SKILL.md`. The skill directs the agent to
this harness for comparable coding-agent evaluations.
