# Runtime-footprint benchmark

`agent-bench.sh` compares two CLI agents on executable size, `--help` startup,
and repeated prompt execution. It is a local-overhead benchmark, not a coding
quality scorer; use `../polyglot/` for independently testable coding tasks.

## Prepare

Build Capstan and install OpenCode independently. Use the same provider, model,
credentials, workspace, prompts, and network conditions for both agents.

```sh
./build.sh
export OPENROUTER_API_KEY=...
```

The runner requires Bash and Python 3. It uses the host `/usr/bin/time` and
selects its available detailed format (`-v` on many Linux systems, `-l` on
macOS). Run the benchmark on an otherwise quiet machine and use alternating
order to reduce warm-cache and rate-limit bias.

## Run

The following produces five runs of each generated neutral prompt per agent.
Results are written under `benchmarks/results/`, which is ignored by Git.

```sh
benchmarks/footprint/agent-bench.sh \
  --capstan ./build/capstan \
  --agent-a-cmd '{bin} run --benchmark --provider openrouter --model deepseek/deepseek-v4-pro --prompt-file {prompt_file} --workdir {workdir}' \
  --opencode opencode \
  --agent-b-cmd 'python3 {repo_root}/benchmarks/footprint/opencode_prompt_file.py --bin {bin} --model openrouter/deepseek/deepseek-v4-pro --workdir {workdir} --prompt-file {prompt_file} --dangerously-skip-permissions' \
  --runs 5 \
  --workdir /path/to/disposable-workspace \
  --out benchmarks/results
```

`--isolated-home` is appropriate only when both agents authenticate exclusively
through environment variables. It prevents local agent state from affecting
runs but also hides credentials stored in home-directory config files.

## Interpret results

`static.csv` records resolved executable path, byte size, and `--help` timing.
`runs.csv` records every task attempt, wall time, user/system CPU, peak RSS, and
captured logs. `summary.md` reports per-task medians and deltas. Preserve raw
CSV, `config.txt`, clean revisions,
agent versions, machine/OS information, provider/model, and exact command
templates with any public claim.

Do not merge these results with the Polyglot score: network wait and provider
behavior dominate task wall time, and the generated prompts do not have an
independent correctness oracle.
