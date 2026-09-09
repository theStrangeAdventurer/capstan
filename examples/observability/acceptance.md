# Observability acceptance evidence

## Session dropdown validation

`python3 test/test_telemetry_grafana.py --dashboard-check --run`: **PASS**.
Without `--run`, this command performs only the static dashboard checks.
The read-only live check inspected Grafana 13.1.0's installed
`/public/plugins/tempo/module.js`: LabelValues is numeric type `1`, and the
variable executor calls `labelValuesQuery(query.label, range)`. The shipped
variables use that schema, not a guessed TraceQL query-variable object.

Both Tempo v2 tag-values endpoints returned HTTP 200 (point-in-time Capstan
counts: one ID, zero names). Exact expanded Session runs and Lifecycle logs
queries passed for All and nonexistent ID/name selections, including punctuation,
quotes and backslashes; negative selections returned no records. Static checks
cover literal matching, no substring false positives, shared predicates,
legacy/benchmark isolation and stable-ID selection with name All for rename
history. Loki selectors still index only service_name, not session identities.

No dashboard was imported or changed on the server. Browser dropdown rendering,
positive name selection and an actual rename-history fixture were **not verified**
in this slice; zero currently returned name options cannot establish them.
Earlier browser evidence below predates this dropdown change. Options exclude
backticks (raw query delimiters); use ID for such names. Backend retention/tag
limits and datasource tag-range settings can limit the options. No secret files,
credentials, outside-workspace files or paid providers were accessed.

## Latest verified result (2026-09-09)

`build/grafana-acceptance-final.json`: **PASS, 10 traces / 53 spans / 106
correlated lifecycle logs**, from `--run --scenario all` on the rebuilt binary.
This supersedes the older point-in-time results below. CLI delay and benchmark
resource fixtures each have seven spans; concurrent children have twelve;
retry/error has fourteen. Native context/roots/failure and injected 503 recovery
also pass. Recovery forwarded the retried log batch and trace batch into real
Tempo/Loki without stopping containers.

Browser: the imported benchmark table with comparison `grafana-acceptance`, task
`synthetic/resource-check`, replicate `r1` found trace
`fba07f2ccbfe8be4a2f0e9f47d3098c5`; selecting its link opened Tempo's seven-span
waterfall, with two approximately 1.3-second model calls. These are synthetic
fixture attempts, not an actual paid paired corpus evaluation.

Checks in this work slice: `make test`, `make test-http-lua`, six mode tests,
23 OTLP tests, 59 Python benchmark tests, and the additional real-binary
trace-free canonical-log consumer test passed. The latter checks native root
metrics/correlation against OTLP and exercises the harness/analyzer without
legacy trace output.

Latest overhead (`build/telemetry-modes-overhead.json`): ten alternating off/on
pairs after warmup, wall medians **120.432 → 122.372 ms (+1.939 ms, +1.61%)**;
RSS medians **28,975,104 → 29,179,904 bytes**. Per-PID CPU samples are retained.
Binary SHA-256: `3379c5ebf9f6fefcdaae1ed5efaa9c316cedd10171f863c1316d39fd77f2a9d7`.
This is whole-process loopback overhead, not production or TUI/ACP latency.
Older measurements below are historical; the current artifact has been replaced.

Final shared-summary verification: `build/grafana-shared-summary-final.json`
passes with 1 trace / 7 spans / 14 logs; `build/grafana-shared-children-final.json`
passes with 1 trace / 12 spans / 24 logs. Runtime-produced root totals are shared
by OTLP, local lifecycle logs and the v1 adapter; canonical consumers use those
explicit totals without reconstructing child durations. Missing historical totals
remain unavailable.

Final-binary overhead recheck: `python3 test/test_telemetry_modes.py
build/capstan --benchmark` passed for binary SHA-256
`56f129defd42a7bd37231a4991e37ca0da58682dde8c31cd5a20d1c7d16f8b99`.
Ten alternating pairs after warmup measured median wall **108.807 → 113.371 ms
(+4.564 ms, +4.20%)**, median RSS **29,016,064 → 29,163,520 bytes**.
`build/telemetry-modes-overhead.json` retains per-PID CPU/RSS samples. This
supersedes the preceding binary's measurement, with the same loopback limitations.

Remaining implementation limits: legacy v1 callbacks remain compatibility,
and exporter diagnostics require an explicit local pull. Pre-root failures and process shutdown do not
have complete shared spans. No actual backend restart, prolonged outage,
production load or live remote authentication test is claimed.

## Historical scope and verdict

Local integration acceptance, **not a production benchmark**. Real Tempo/Loki
storage was queried through Grafana on loopback; model responses and native
contexts are synthetic. No paid model, tool credentials, user configuration or
secret files are needed. Existing compatibility tests/assertions were preserved.

### Real backend delivery recovery through an injected relay outage

`python3 test/test_telemetry_grafana.py --run --scenario backend-recovery`:
**PASS**, trace `70afff99b7ba037cd5cad107c11893e6`, **1 trace / 3 spans / 6
correlated lifecycle logs** in actual Tempo/Loki queried through Grafana.
The existing native `failure` fixture runs with a credential-free environment;
no model calls, evaluations, user configuration, or secret files are used.

The loopback relay rejects the first export request with **503** before forwarding
anything, then forwards original protobuf bytes to the actual collector. Measured
counts: **3 HTTP attempts, 1 rejected request, 2 successfully forwarded requests**:
logs **2 attempts / 1 rejection / 1 success**, traces **1 attempt / 0 rejections /
1 success**. The rejected log batch contains all six lifecycle records. Its retry
must be byte-identical. Assertions check unique successfully forwarded span IDs
and lifecycle identities, exact backend counts, parents, snapshots, outcomes,
and exactly one start/finish per span in Loki. No duplicate successful records
were observed; this is not a general exactly-once delivery guarantee.

An initial version rejecting the first request of *each* signal failed with
`native fixture failed or reported exporter losses`: native cleanup has a
2-second deadline and two serialized retries each require at least 1 second.
That failed run is retained here as evidence, not counted as success. The final
scenario injects one initial failed batch, covering recovery within that budget.
`Retry-After: 0` does not bypass the exporter's minimum retry backoff.

Restrictions: no containers were stopped/restarted, no backend settings changed,
and no paid calls made. This is an injected collector-facing HTTP outage followed
by **real backend delivery**, not a collector process crash/restart, prolonged
outage, simultaneous failure of both signals, or lost-acknowledgement test. Raw
responses/payloads remain in memory; only safe IDs/counts are printed. The passing
result was printed, not saved as a new artifact. `--scenario all` includes this
scenario but was not rerun.

### Real CLI concurrent-subagent and retry/error acceptance

Added `cli-concurrent` and `cli-retry-error` using the existing
`test_telemetry_modes.fixture`, disposable configuration, and its threaded
loopback mock provider. Both run the actual `build/capstan run` executable;
no runtime code, provider credentials, or paid calls are involved.

- `python3 test/test_telemetry_grafana.py --run --scenario cli-concurrent`:
  **PASS**, trace `162e20a427566585cf665ea655a6a052`, **11 spans / 22 correlated
  lifecycle logs**. Saved in `build/grafana-cli-concurrent-acceptance.json`.
  Two child HTTP handlers rendezvous before a 1.25-second wait, so sequential
  dispatch cannot satisfy the fixture. Checks prove overlapping child runs,
  tool → child → model ownership, containment, successful child results in input
  order, and exactly one start/finish per exported span in Loki.
- `cli-retry-error`: **PASS on diagnostic rerun** by calling
  `cli_acceptance(ROOT / 'build/capstan', scenario='cli-retry-error')` directly
  against the same localhost backend. One child receives HTTP 503 then succeeds
  on its second model request; the other receives HTTP 400 and is not retried.
  Assertions check exact request counts, five model spans, two child runs with
  success/error outcomes, one `stream_transient_error` retry operation owned by
  the recovered child, and failed-model → retry → successful-model ordering.
  The parent receives mixed child results and successfully synthesizes its answer.
  Tempo snapshots/parents and Loki exactly-once lifecycle/outcome correlation
  are checked, along with session ID/name search.

The retry/error command's preceding run failed with an unclassified `Error`;
its cause was not reproduced by the diagnostic rerun. No successful retry JSON
artifact was retained, and uninterrupted repeated-run reliability is not claimed.
An initial fixture assertion also incorrectly treated the parsed API error
message as private raw body data; the fixture now puts its forbidden sentinel
in a separate `private_debug` field. Parsed actionable messages remain allowed;
raw-body sentinel export and copying into the parent request remain forbidden.

`--scenario all` now includes both new scenarios. The full combined invocation
was not rerun in this slice. Existing CLI delay/resource expectations are now
six spans (including `session_save`), so the older five-span evidence below is
historical, not the current expected structure.

### Earlier full backend acceptance

`python3 test/test_telemetry_grafana.py --run --scenario all`: **PASS** against
Grafana `127.0.0.1:3000` and collector `127.0.0.1:4318`, using existing workspace
binaries (not rebuilt in this work slice). Measured **7 traces, 20 spans, 40
correlated lifecycle logs**; uncorrelated startup logs are excluded. Result was
printed to the terminal, not saved as a new JSON artifact.

| Scenario | Trace ID | Spans / logs |
| --- | --- | --- |
| CLI controlled delay | `c1406fbb60c847410fd193c18c4d4778` | 5 / 10 |
| Benchmark resources | `3a068623c1a9f238d38ce1b3fbbcfb90` | 5 / 10 |
| Immutable context | `6beb6eb88063b14d90e1169b313117b1` | 3 / 6 |
| Independent roots | `b5cf4f67aabf1176dbf0d6515640639b` | 2 / 4 |
| Independent roots | `5481fb0ce9cf316d80ed5fef88fc8efd` | 1 / 2 |
| Independent roots | `03f4de34548c434d7e48a6fc2fda7118` | 1 / 2 |
| Synthetic failure/cancellation | `ecdcb785b311723499aae426fd3bdb20` | 3 / 6 |

The new `benchmark-resources` scenario imports the canonical
`run_eval.telemetry_environment` adapter and supplies six synthetic identity
fields through `OTEL_RESOURCE_ATTRIBUTES`: `benchmark.attempt_id`, `config_id`,
`harness_sha256`, `task`, `comparison_id`, and `replicate_id` (all use the
`benchmark.` prefix). It checks the exact benchmark namespace in every Tempo
resource batch and every correlated Loki lifecycle record, including normalized
Loki field names. Injected stale attempt identity and an unwanted inherited
benchmark field must disappear. Existing CLI timing/session/correlation checks
remain active. The fixture hash is deliberately synthetic, not a claim about
the harness revision. This tests resource **identity**, not CPU/RSS metric export,
scoring, real model evaluations, or `--benchmark` isolated mode. No new exporter
or telemetry policy was introduced.

### Historical evidence (before the fresh run above)

- Prior native `context` + `roots` acceptance: PASS reported by the orchestrator
  (7 spans, 14 lifecycle logs across 4 traces expected by the harness). No saved
  native result artifact was available in this work slice; do not mistake these
  expected counts for newly measured counts. It was not rerun.
- Prior real CLI acceptance: verified saved `build/grafana-cli-acceptance.json`,
  PASS, trace `a5da4705e57b7a0a5ae60de08d53c077`, 5 spans, 10 lifecycle logs.
  Two controlled 1.25-second model waits, one `file_read`, and one permission
  decision. Harness checks session ID/name search in Tempo, exact IDs/parents and
  immutable attributes, root containment, model durations, and Loki correlation
  and lifecycle timing. This successful scenario was not rerun.
- New distinct native failure/cancellation acceptance: PASS in
  `build/grafana-failure-acceptance.json`, trace
  `18ce4546b6a08d88a803527bd21592ef`, 3 spans and 6 lifecycle logs. Existing native
  driver emits successful root, failed model, cancelled tool. Tempo snapshots and
  Loki finish outcomes match exactly. This is **synthetic application failure
  exported to a healthy real backend**, not a backend outage or real CLI failure.

## Reproduction

With the already configured local Grafana/Tempo/Loki stack described in
[grafana.md](grafana.md), and project dependencies built:

```sh
make build/capstan build/test_telemetry_native
# Choose only the gap being checked; these commands export synthetic fixtures.
python3 test/test_telemetry_grafana.py --run --scenario native
python3 test/test_telemetry_grafana.py --run --scenario cli-delay
python3 test/test_telemetry_grafana.py --run --scenario failure
python3 test/test_telemetry_grafana.py --run --scenario benchmark-resources
python3 test/test_telemetry_grafana.py --run --scenario cli-concurrent
python3 test/test_telemetry_grafana.py --run --scenario cli-retry-error
python3 test/test_telemetry_grafana.py --run --scenario backend-recovery
# --scenario all combines all scenarios, if a full fresh acceptance is required.
```

The relay forwards original protobuf payloads to `127.0.0.1:4318`, rejects fixture
private content, and queries only generated trace IDs/unique session names through
Grafana at `127.0.0.1:3000`. Raw backend responses stay in memory. JSON output can
be redirected into `build/`; those generated artifacts are not permanent evidence
unless retained by the caller. The runner defaults to SKIP without `--run`.

## Measured local overhead

```sh
python3 test/test_telemetry_modes.py build/capstan --benchmark
```

Measured successfully on macOS 26.5.1 arm64, Python 3.14.6; binary SHA-256:
`f6f70f24150d36b6d71462ae7d0dbb652e623b3295fd9612ff90631211205dba`.
Full per-process samples: `build/telemetry-modes-overhead.json`.

| Median, excluding warmup | Telemetry off | Telemetry on |
| --- | ---: | ---: |
| Wall time | 105.999 ms | 115.739 ms |
| Peak RSS (bytes) | 28,483,584 | 29,171,712 |

Wall delta: +9.740 ms (+9.19%); difference of RSS medians: +688,128 bytes.
These are observations, not performance thresholds or causal allocation estimates.

Method: one warmup off/on pair then ten pairs with alternating ordering; fresh
isolated CLI sessions, two loopback SSE requests and one fixture file read per
process. Each run checks the answer and expected export structure (or no exports
when disabled). Setup/teardown of fixture servers is excluded. `os.wait4(pid)`
provides **that process's** user/system CPU and peak RSS, not cumulative
`RUSAGE_CHILDREN` peaks. macOS bytes and Linux KiB are normalized to bytes. Output
uses temporary files to avoid pipe deadlocks and accidental early reaping;
timeouts kill and reap the child. Unsupported platforms fail explicitly.

Whole-process wall includes startup, local logs, export at shutdown and 1ms wait
polling granularity (scheduling may add more). Whole-process RSS is not telemetry
allocation size. Shared-host noise, fixture transport, and short runs prevent
extrapolation to real models, production workloads, sustained throughput or
TUI/ACP latency. No production or paid benchmark was run.

## Coverage and actual checks

- `python3 test/test_telemetry_modes.py build/capstan MeasurementTests`: **2 PASS**
  (per-process resources/output/nonzero exit and timeout cleanup).
- `--benchmark`: **PASS**, 22 fresh fixture processes with per-PID RSS and existing
  answer/export assertions.
- `vendor/lua-5.5.0/src/lua test/test_runtime_timeline.lua`: **PASS**. Actual Lua
  run/tool paths with mocked clock/transport exercise permission wait/deny/error,
  guard stop, queued subagents, retry, queued cancellation, optional token usage,
  and model transport retry. All recorded spans must finish once.
- `vendor/lua-5.5.0/src/lua test/test_telemetry.lua`: **16 PASS**. Explicit parent
  ownership, interleaved siblings, descendant cancellation, observer/native
  failure isolation and shutdown behavior.
- Inspected existing C-provider runtime fixture in `test/test_telemetry.lua`:
  interleaved subagent completions, cancellation with late callbacks ignored,
  second model attempt under the same run, and exactly-once settlement. This
  embedded branch is not executed by the standalone 16-test invocation; its
  prior passing status comes from the orchestrator, not a new run here.
- Inspected native driver + `test/test_telemetry_otlp.py`: cancelled/error outcome
  serialization, local adapter write failure isolation, exporter retry payload
  identity/Retry-After, retry limits and cleanup deadline cases. Prior passing
  status supplied by orchestrator; not redundantly rerun here.
- `python3 test/test_telemetry_grafana.py --run --scenario failure`: **PASS** against
  real backend, as detailed above.

## Remaining gaps

- Actual CLI cancellation/queued cancellation, whole-child retries after model
  retry exhaustion, and terminal orchestrator failure remain unverified against
  the real backend. The new retry/error scenario tests stream retry and a failed
  child inside a successful orchestrator, not these other transitions.
- The retry/error acceptance had an unclassified intermittent `Error` before a
  successful diagnostic rerun; its source and repeated-run stability remain open.
- Real collector/backend process outage and restart recovery, prolonged outages,
  load, retention, remote TLS/authentication and production deployment remain
  unverified. The relay-injected 503 recovery above verifies real backend delivery
  without disrupting user containers; it does not exercise process restart.
- CLI/TUI/ACP mode tests previously passed per orchestrator. TUI harness terminates
  after idle export; it does not prove graceful shutdown flushing.
- Linux RSS unit conversion is implemented but this measurement ran on macOS only.
- Grafana dashboard rendering and human visual timeline inspection are not claimed
  by the API-based acceptance. No production performance conclusion is justified.
