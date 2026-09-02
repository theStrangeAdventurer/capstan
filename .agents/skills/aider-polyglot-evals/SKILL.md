---
name: aider-polyglot-evals
description: Run, calibrate, compare, or inspect reproducible coding-agent evaluations using the open-source Aider Polyglot corpus. Use for a 12-task Capstan or OpenCode comparison, per-task upstream-test results, agent CPU/RSS measurement, or a fair model/agent benchmark without task-specific instructions.
---

# Aider Polyglot evaluations

Use `benchmarks/polyglot/scripts/run_eval.py` as the canonical harness. Keep
the corpus commit, `mini-v2` task list, public prompt construction, timeouts,
and upstream scorer unchanged while comparing configurations.

## Prepare

1. From the repository root, run:

   ```sh
   ./benchmarks/polyglot/bootstrap.sh
   ./build.sh
   ```

   Bootstrap clones `Aider-AI/polyglot-benchmark` into
   `benchmarks/work/polyglot-benchmark`, checks all required language
toolchains, and checks out the pinned corpus revision.
2. Before spending model calls, run `git status --short` in both the Capstan
   repository and corpus. Refuse a dirty corpus. Record a dirty Capstan
   worktree as non-reproducible exploratory work, not a publishable result.
3. Put provider credentials in the environment, never in a committed file.
4. Confirm an agent's authentication with one neutral request before a full
   suite. Use fresh output directories outside the corpus.

## Canonical comparison

For the published-style local comparison, use direct DeepSeek V4 Pro, medium
reasoning, 240-second agent timeout, and sequential full suites:

- Capstan agent command:
  ```sh
  {repo_root}/build/capstan run --benchmark --no-wiki --provider deepseek --model deepseek-v4-pro --profile implement --reasoning-effort medium --max-turns 40 --prompt-file {prompt_file} --workdir {workdir} --workspace {workdir} --json --trace-file {trace_file}
  ```
- OpenCode agent command:
  ```sh
  python3 {repo_root}/benchmarks/polyglot/scripts/run_opencode.py --prompt-file {prompt_file} --workdir {workdir} --model deepseek/deepseek-v4-pro --variant medium
  ```

Pass either command as `run_eval.py --agent-command`. First run the complete
harness command with `--dry-run`; only remove it after checking corpus revision,
task matrix, output directory, command, and model. The OpenRouter alternatives
are documented separately in `benchmarks/polyglot/README.md`; do not mix direct
DeepSeek and OpenRouter outputs under one `--comparison-id`.

Run each complete 12-task suite three times in fresh `r1`, `r2`, and `r3`
output directories. Run suites sequentially; do not reuse partial output,
selectively rerun failed tasks, or run comparison configurations concurrently.
Balance provider load over time while keeping execution sequential:

```text
r1: Capstan -> OpenCode
r2: OpenCode -> Capstan
r3: Capstan -> OpenCode
```

For every Capstan command, include `--trace-file {trace_file}`. For both agents,
pass the same `--replicate-id rN` for corresponding repetitions and the same
`--comparison-id` only when provider route, model, reasoning setting, corpus,
task matrix, and timeouts are comparable. The documented commands in
`benchmarks/polyglot/README.md` are the source of truth for these flags and
placeholders.

## Evaluation integrity

- Give agents only the public exercise prompt. Never add solution paths,
  implementation hints, task-specific instructions, or agent-specific help.
- Do not expose `.meta` or `.docs` to the agent workspace.
- Score only upstream tests in the clean scoring copy. Agent exit status or
  self-reported success is not a pass.
- Keep test timeout and task selection fixed. A changed task list, corpus,
  timeout, model, or harness is a new benchmark configuration.
- Treat `agent_timeout`, `agent_error`, `test_failure`, and `harness_error` as
  distinct outcomes.
- If the harness is defective, fix it and invalidate affected runs. Never edit
  CSV or summary results manually.

## Results

After all repetitions finish, run the trace analyzer over every corresponding
output directory, for example:

```sh
python3 benchmarks/polyglot/scripts/analyze_traces.py \
  --capstan /tmp/capstan-polyglot-r1 \
  --capstan /tmp/capstan-polyglot-r2 \
  --capstan /tmp/capstan-polyglot-r3 \
  --other /tmp/opencode-polyglot-r1 \
  --other /tmp/opencode-polyglot-r2 \
  --other /tmp/opencode-polyglot-r3
```

Treat process wall time as authoritative for all attempts, including timeouts
and crashes. Use model/tool/permission/subagent/unattributed breakdowns only
from complete, process-consistent Capstan traces. Preserve `.partial` traces for
diagnosis, but do not include partial, corrupt, or inconsistent telemetry in
breakdown averages. Runtime logs are diagnostic context and never replace the
per-task trace. Preserve OpenCode's raw event logs; compare normalized metrics
without treating format differences as performance differences.

Report score, grouped failure modes, per-task and aggregate wall time, CPU,
peak RSS, corpus commit, clean revisions, agent versions, machine details,
provider/model/reasoning settings, and paths to raw output. Also report model,
tool, permission, subagent, and unattributed time; model request and tool-call
counts; trace/metric coverage; and counts of partial, corrupt, and inconsistent
traces. State that results are workload- and environment-specific; they are not
a universal ranking.

Use `benchmarks/polyglot/promptfoo/` only when an HTML report is requested.
Promptfoo is an optional orchestration layer; `run_eval.py` owns task isolation
and scoring. Read its README before use and keep a filled local config only at
its ignored `benchmarks/polyglot/promptfoo/config.local.yaml` path.
