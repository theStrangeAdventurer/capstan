# Benchmarks

## Purpose

Capstan maintains reproducible, workload-specific comparisons rather than a
single universal performance claim. Benchmark tooling is available to every
repository clone under `benchmarks/`; it is not part of ordinary build or test
targets because it requires external toolchains, network access, and model
credentials.

## Aider Polyglot quality benchmark

`benchmarks/polyglot/` owns the canonical `mini-v2` Aider Polyglot evaluation:

- `bootstrap.sh` clones `Aider-AI/polyglot-benchmark` under
  `benchmarks/work/` and detaches it at the pinned corpus commit;
- `scripts/run_eval.py` owns public-prompt construction, isolated agent and
  scoring worktrees, upstream-test execution, timeouts, result classification,
  and agent-process resource measurement. Peak agent RSS is sampled every 50
  ms from only the primary agent PID, excluding child tool processes; raw
  `wait4` peak RSS is retained separately for workload diagnostics;
- `scripts/run_opencode.py` adapts OpenCode while isolating extension surfaces.
  For OpenRouter it requires `--reasoning-effort` to make a temporary,
  per-model `reasoning.effort` variant; a bare OpenCode `--variant` may be
  absent from the current model catalog and silently omit the setting;
- `.agents/skills/aider-polyglot-evals/SKILL.md` tells any Capstan agent using a
  local clone when and how to run the fixed suite;
- `promptfoo/` is an optional HTML-report layer and must not alter scoring.

The corpus and outputs are intentionally ignored. The corpus contains the
upstream test data; copying it into Capstan would make updates and checkout
integrity harder to manage. Large run logs and provider-specific local paths
must not be committed.

Comparisons must preserve corpus revision, task list, public prompt,
agent/test timeouts, model capability, reasoning setting, provider route, and
sequential execution. A changed value is a new named benchmark configuration.
Each published result records both clean agent revisions, versions, machine
information, provider/model settings, repetition count, raw result data, and
limitations.

### Telemetry migration boundary

The harness writes a random `attempt_id` plus task/comparison/replicate identity
in `tasks/<task>/attempt.json` before agent launch, and retains it in each result.
The sidecar survives harness interruption; it does not imply a completed result.
Process wall/CPU/RSS and upstream scoring remain harness-owned and unchanged.

The legacy trace reader accepts one additive `run.context` event whose data is
`run_id` (common lifecycle invocation ID), `trace_id` (nonzero lowercase 32-digit
hex), and optional nonempty `session_id`. These are copied into telemetry
`correlation` alongside `legacy_run_id`, including validated partial prefixes.
Legacy writer IDs are not distributed trace IDs. Missing common IDs remain
absent; partial correlations never make terminal metrics trustworthy. Duplicate
or malformed context events invalidate telemetry, not process measurements.
Existing traces without this event remain supported and pairing is unchanged.

The runtime now supplies `on_run_start(context)` before validation, hooks and
transport. The CLI compatibility wiring emits `run.context` at root start when
native context exists, including optional `span_id` (now validated as nonzero
lowercase 16-digit hex and copied by the benchmark consumer). Model/tool completion computes duration once for both native
measurements and legacy observers, so their reported duration values are exact
matches, not independent clock samples. Legacy trace publication remains strict.

Optional backend navigation: pass analyzer `--trace-url-template
'https://tempo.example/trace/{trace_id}'` (optionally containing `{span_id}`).
This appends a `backend_traces` TSV column containing a JSON list of links with
replicate IDs and telemetry statuses. URLs are operator supplied HTTP(S) templates,
without userinfo; do not put credentials in them. No backend requests or discovery
occur. Only validated common context creates links; legacy IDs never substitute.
Missing required span IDs omit links. Partial and process-inconsistent traces may
have diagnostic links but remain excluded from metric averages; corrupt traces
have no links. Backend links remain opt-in and legacy parser compatibility is preserved.
The common validator owns ID validation for both parser and analyzer adapters.

Compatibility boundaries are explicit: isolated `--benchmark` states still deny
native context/export. Normal runtime identity and the local `capstan.log.v1`
lifecycle sink are independent of OTLP. The optional `--canonical-log` adapter
reads producer-owned root aggregates from that sink for non-isolated diagnosis;
old or interrupted records without those measurements remain absent. Explicit `--telemetry-context` adapts
harness identity into OTel resources for non-isolated diagnostic commands only;
`--session-id {attempt_id}` provides an explicit session link. Neither enables
isolated benchmark telemetry. CLI startup/final-save spans exist in the common
lifecycle. Runtime computes counts/breakdowns once; native sinks and the legacy
v1 adapter reuse them, while process timing/publication remains CLI-owned.
Backend links do not prove backend retention or paired-attempt navigation.
`--trace-file` is required only for published isolated comparisons that need
legacy breakdowns, not for native backend or canonical-log diagnosis. Its
explicit fail-closed publication and process reconciliation remain unchanged. Wall time remains
process-authoritative; absent metrics remain absent, never synthesized as zero.

Coverage: `benchmarks/polyglot/scripts/test_run_eval.py` tests complete/partial
correlation and malformed/duplicate context rejection. `test/test_telemetry.lua`
tests root-start delivery and exact shared model/tool durations.
`make test-telemetry-modes` checks real CLI trace-to-OTLP IDs and invokes the
benchmark consumer against that trace; it uses ordinary CLI mode, not isolated
`--benchmark` execution. Backend workflow and comparable off/on process overhead
remain separate acceptance checks.

## Runtime-footprint benchmark

`benchmarks/footprint/agent-bench.sh` measures executable size, `--help`
startup, and repeated prompt execution. Its prompts have no independent
correctness oracle, so the benchmark is evidence about local client overhead,
not coding quality. It must use the same provider/model/credentials, workspace,
prompt set, and network conditions for compared agents. Alternating order is
the default to reduce warm-cache and rate-limit bias.

## Security and isolation

Benchmark runs use disposable task copies and should use disposable workspaces.
Credentials are supplied through the environment or the user's normal agent
authentication, never benchmark files. Filled Promptfoo config files remain
local because they can expose local paths. The `--benchmark` Capstan mode is
used for agent runs to exclude project instructions, user skills, hooks, MCP,
Wiki, and user plugins from evaluations.

## Validation

- `python3 -m unittest benchmarks/polyglot/promptfoo/test_adapter.py` validates
  the optional report adapter without model calls.
- `./benchmarks/polyglot/bootstrap.sh --check` validates a prepared corpus and
  required toolchains.
- `--dry-run` on `run_eval.py` verifies the task matrix and command before
  provider calls.
- Shell scripts are checked with `bash -n`.

### Optional canonical file-log ingestion

`run_eval.py --canonical-log PATH` opts into the existing `capstan.log.v1`
file exporter as the telemetry input instead of the legacy per-task trace.
The command must explicitly include `--session-id {attempt_id}` and must not
include `--benchmark` (rejected before corpus access); the harness neither
configures an exporter nor adds instrumentation. `harness_sha256` fingerprints
both `run_eval.py` and its `canonical_logs.py` dependency. The file may be shared:
selection uses that generated session identity and must resolve to exactly one
trace/run. The Python `load_log_summary` adapter also accepts explicit trace,
run, and session selectors. Ambiguous or absent identities fail closed.

Lifecycle records pair by trace/span identity, with stable run, session, name,
and native `parent.span_id` linkage on start and finish. Root records omit the
parent; the one `agent.run` root must own the run identity. Duplicate events,
invalid outcomes, unmatched finishes, missing/cyclic parents, changed identity,
and malformed/truncated JSONL are corrupt; unfinished starts are partial.
Event order must contain ordinary child work within its parent's lifetime:
a child cannot start before its parent or after its parent finishes, and a parent
cannot finish with active children. Parallel siblings may overlap and finish in
either order; no stack ordering or timestamp-derived duration is imposed.
Unrelated diagnostic records are ignored, but malformed shared input is rejected.
The explicit exception is `operation=session_save` after a completed `agent.run`
root: persistence retains that root's context and identity. This does not allow
arbitrary late children, or a child that starts before and outlives its parent.

Native lifecycle encoding supplies parent linkage to both file and OTLP logs;
caller attributes cannot supply or override it.

A complete canonical lifecycle supplies the observed root outcome, correlation,
explicitly supplied root `duration_ms`/`turns`, and runtime-owned counts/breakdowns
when present. Flat `request_count`/`tool_count` map to analyzer counts;
model/tool/permission/subagent/unattributed/overlap milliseconds map to its
breakdown. No intended process exit code or missing measurements are inferred. The real-binary
`ModeTests.test_cli_trace_free_canonical_log_summary` checks these fields against
OTLP and feeds them through the harness and analyzer without `--trace-file`. Process wall time, timeout, exit status, resource measurements and upstream
scoring remain authoritative. Timeout or outcome/exit disagreement marks the
telemetry inconsistent. Unknown metrics remain absent (`N/A`, zero metric
coverage), not inferred from wall-clock log timestamps or fabricated as zero.
The analyzer appends `duration_ms`, `turns`, and `overlap_ms` averages, each
followed by its own `<name>_coverage` column (measured/expected replicates).
These columns use explicit measurements from complete, process-consistent
telemetry independently of the existing all-breakdown `metrics` coverage.
Millisecond columns stay in milliseconds; `turns` is a nonnegative integer per
run, though its average may be fractional. Missing measurements display `N/A`
and `0/N`; an explicit zero is measured and counts toward coverage. Older legacy
and canonical records need not supply these fields. Parser/analyzer unit tests
cover parent lifetime violations, overlapping siblings, invalid root metrics,
and missing-versus-zero coverage without running evaluations.
Legacy `capstan.trace.v1` ingestion remains the default compatibility adapter;
there is no automatic fallback from corrupt canonical logs.

Limitations: this adapter consumes local JSONL, not OTLP protobuf/backend queries.
Paired events cannot prove absence of an entirely dropped span. Rotated files
must be supplied as one complete ordered input; no rotation discovery is done.
Canonical totals describe completed callbacks only. Forced owner/shutdown closure
retains completed counters but can lack duration/residual totals; older logs are
supported without manufacturing measurements. Integers too large for finite
numeric measurements mark telemetry corrupt rather than aborting upstream scoring.
Tests: `python3 -m unittest discover -s benchmarks/polyglot/scripts -p 'test_*.py'`
and `make test-telemetry` (loopback-only native contract tests).
