# Aider Polyglot benchmark

This directory contains Capstan's reproducible harness for the fixed
[Aider Polyglot](https://github.com/Aider-AI/polyglot-benchmark) `mini-v2`
coding-agent suite. It evaluates 12 Exercism tasks across C++, Go, Java,
JavaScript, Python, and Rust. The corpus itself is not vendored: it is cloned
at a pinned commit into `benchmarks/work/polyglot-benchmark`.

Use this benchmark to compare agents or model configurations. It is not part
of `make test`: it needs network access, language toolchains, provider
credentials, and can consume substantial model time.

## Prepare

From the repository root:

```sh
./benchmarks/polyglot/bootstrap.sh
./build.sh
```

The bootstrap script clones the corpus and verifies `git`, `python3`, `cmake`,
`go`, `java`, `node`, `npm`, and `cargo`. It rejects a dirty corpus checkout
and detaches it at commit:

```text
7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f
```

Use an existing checkout instead of the default location with
`--corpus /path/to/polyglot-benchmark`. `--check` only validates a prepared
checkout and toolchains.

Set the provider credential in the environment. The canonical historical
comparison uses direct DeepSeek V4 Pro with medium reasoning:

```sh
export DEEPSEEK_API_KEY=...
```

Install OpenCode separately and put it on `PATH` only when comparing against
it. Do not put credentials in benchmark configs or result files.

### OpenRouter configuration

For an OpenRouter comparison, use the same `deepseek/deepseek-v4-pro` model ID
for Capstan and the OpenCode adapter below, with:

```sh
export OPENROUTER_API_KEY=...
```

Pass OpenCode `--reasoning-effort medium`, not just `--variant medium`. The
adapter supplies a temporary per-model override mapping that setting to the
OpenRouter request body `reasoning.effort: "medium"`; OpenCode otherwise may
silently ignore a variant unavailable in its current model catalog.

## Run Capstan

This OpenRouter example uses the same route and model as the OpenCode command
below. Inspect the complete plan first; this does not call a model:

```sh
python3 benchmarks/polyglot/scripts/run_eval.py \
  --corpus benchmarks/work/polyglot-benchmark \
  --output /tmp/capstan-polyglot-r1 \
  --timeout 240 \
  --agent-command '{repo_root}/build/capstan run --benchmark --no-wiki --provider openrouter --model deepseek/deepseek-v4-pro --profile implement --reasoning-effort medium --max-turns 40 --prompt-file {prompt_file} --workdir {workdir} --workspace {workdir} --json' \
  --dry-run
```

`{repo_root}` expands to the absolute path of this Capstan clone, so the command
works from each temporary task workspace. Remove `--dry-run` to execute the 12
tasks. Use a fresh, empty `--output` directory for every run. Repeat complete
runs in fresh `r1`, `r2`, and `r3` directories; do not selectively rerun failed
tasks.

The harness gives the agent only public task instructions, removes `.meta` and
`.docs` from its workspace, copies only declared solution files into a fresh
scoring tree, and scores that tree using upstream tests. An agent cannot pass
by modifying tests or accessing the example solution.

## Run OpenCode

Use the same corpus, provider route, model capability, reasoning setting,
timeout, and sequential execution. The adapter uses OpenCode's `build` agent,
creates a fresh OpenCode config directory, disables project/external extension
surfaces, and limits the run to the disposable task workspace.

```sh
python3 benchmarks/polyglot/scripts/run_eval.py \
  --corpus benchmarks/work/polyglot-benchmark \
  --output /tmp/opencode-polyglot-r1 \
  --timeout 240 \
  --agent-command 'python3 {repo_root}/benchmarks/polyglot/scripts/run_opencode.py --prompt-file {prompt_file} --workdir {workdir} --model openrouter/deepseek/deepseek-v4-pro --reasoning-effort medium' \
  --dry-run
```

Remove `--dry-run` after inspecting the plan.

## Results and reporting

Each output directory contains `metadata.json`, `results.json`, `summary.md`,
and per-task agent and validator logs. `run_eval.py` measures the complete
agent process tree with `wait4`; it records agent wall time, user/system CPU,
and peak RSS while excluding the later validator process.

Record the following with results: clean Capstan revision, OpenCode version,
corpus commit, OS/CPU, provider/model, reasoning setting, timeouts, task count,
and repetition count. Existing published raw CSV files live one directory up;
keep new large logs and corpus checkouts out of Git.

## Optional Promptfoo report

`promptfoo/` provides a report adapter around the same harness. It is optional;
the direct runner above remains the canonical scorer. Copy
`promptfoo/config.example.yaml` to the ignored
`promptfoo/config.local.yaml`, replace every placeholder with absolute paths,
then run:

```sh
benchmarks/polyglot/promptfoo/run.sh \
  --config benchmarks/polyglot/promptfoo/config.local.yaml
```

Promptfoo requires `npx`; the wrapper pins its version and disables telemetry
and cache. Do not commit the filled config because it may reveal local paths.
