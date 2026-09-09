# Grafana: inspecting Capstan telemetry

## Access and data sources

The locally inspected stack exposes Grafana at http://127.0.0.1:3000 with
Tempo UID `tempo` and Loki UID `loki`. These are deployment-specific values.

In the earlier read-only inspection, anonymous access had the Viewer role.
Explore was enabled globally, but `/explore` redirected that browser to Home. The
existing dashboard's trace links also target Explore. Sign in with an account
authorized to use Explore; do not solve this by making anonymous users admins.
API read access alone does not establish Explore or dashboard-write permission.

## Reproducible dashboard

Import [`capstan-dashboard.json`](capstan-dashboard.json) with **Dashboards → New
→ Import → Upload dashboard JSON file**, using an account authorized to create
dashboards. The template has its own UID, `capstan-session-diagnostics`, so it
does not replace the existing provisioned `capstan-local` dashboard. No API write
or permission change is required to inspect the existing backend.

Choose the Tempo and Loki data sources in the dashboard selectors. Defaults are
UIDs `tempo` and `loki`; the selectors make other deployments usable without
editing panel queries. The template assumes resource `service.name = "capstan"`
and Loki's standard OTLP normalization (`session.id` → `session_id`). Adjust the
service selector if your collector renames it. Requires Tempo TraceQL and Loki
structured metadata support. Queries deliberately omit `select`: its mixed
response frames crashed the browser table renderer in the inspected stack.

- **All runs** shows up to 100 traces containing `agent.run` or `run` spans,
  including old and unsaved executions. It deliberately ignores session filters.
- **Session runs** uses searchable **Session name** and **Session ID** Tempo
  dropdowns, not textboxes. Both selections filter the table and lifecycle logs.
  A persisted ID is required in this table; **All** names includes unnamed runs.
  Results are traces, not one row per child invocation. Expand a trace for its
  session ID/name and other attributes.
- **Lifecycle logs** applies those same selections plus **Trace ID** (a regex).
  Reset session selectors to **All**, Trace ID to `.*`, and widen the time range
  before concluding data is missing. Default logs include absent session metadata.

Type in a dropdown to search exported values. Names require the opt-in below;
IDs work without it. Values are literal, with anchored Grafana `:regex` escaping,
so punctuation needs no manual escaping. Equal names may represent multiple IDs.
For rename history, select the stable ID and reset **Session name to All**: a
specific name intentionally limits results to that launch-time snapshot. The ID
selector is independent of the name selector, not a name-to-ID mapping.

Tempo query-variable schema is `{"type":1,"label":"span.session.name"}` (or
`span.session.id`), with the chosen Tempo datasource, single selection, All and
refresh on time-range change. The installed plugin's label-values API does not
accept a service filter in this variable schema; options can include other
services, while panels remain scoped to Capstan. Options depend on retention,
backend tag limits and datasource tag-time-range settings, not the local session
registry. Widen time/refresh if needed; enabling names does not backfill history.

Queries use RE2 in raw backtick strings. Dropdown option filtering excludes
values containing a backtick to protect the query delimiter; use the stable ID
for such a name. Do not bypass that guard via dashboard URL variables. Other
textboxes remain RE2 expressions and report invalid-query errors. Session IDs,
names and trace IDs stay Loki structured metadata, never indexed labels.
Queries are bounded by time and result limits, not exhaustive run counters.

### Benchmark comparison navigation

**Benchmark runs** applies optional **Comparison ID**, **Task** and **Replicate
ID** RE2 filters to the canonical resource attributes
`benchmark.comparison_id`, `benchmark.task` and `benchmark.replicate_id`.
Each default `.*` includes missing values; a specific filter excludes records
missing that field. All three predicates must match. These selectors affect only
this new table: All runs, Session runs and Lifecycle logs retain their existing
queries and fallback behavior. Session filters do not constrain benchmark runs.

Use the harness's opt-in `--telemetry-context` with native export already enabled
to supply these process resources; see the [observability spec](../../specs/observability.md).
No alternate ingestion or identity policy is added here. The table shows at most
100 traces, not one row per attempt or child run. Select its linked trace ID for
Tempo's waterfall and inspect resource attributes for attempt/configuration
identity. To use the existing log panel, paste that ID into **Trace ID** and
reset both session selectors to **All**. Benchmark selectors do not filter logs
implicitly. The existing Tempo/Loki correlation links also remain available,
subject to Explore permissions.

This is navigation, not a paired benchmark scorecard: equal comparison/task/
replicate labels alone do not establish compatible configurations. Pairing and
external process wall/CPU/RSS remain harness-owned. Span duration is not process
wall time; absent measurements are not zero. Legacy data remains accessible
without benchmark resources; enabling context does not backfill it.

### Exporter diagnostics are local-only

`capstan.telemetry.diagnostics()` is an explicit local pull of the in-memory
snapshot (`dropped`, `rejected`, `failed`, `malformed`, `configuration_errors`,
`queued`, and boolean `enabled`). `queued` excludes in-flight records; isolated
states receive `nil`. A permitted pull also attempts a synchronous local
`telemetry` / `exporter.diagnostics` structured log event without trace/span IDs.
It is not autonomous polling, and exporter polling/shutdown does not emit it.
These diagnostics are currently **not exported to Tempo/Loki or available via
the backend**. This dashboard therefore has no fabricated exporter-loss metrics
or diagnostic panel. Empty backend searches do not prove zero drops; inspect the
explicit local snapshot when diagnosing delivery, including exporter failure.

### Session identity and privacy

Native export must already be enabled and directed to your OTLP collector.
Session IDs and run IDs are diagnostic identities; human-readable session names
require the explicit configuration opt-in:

```lua
observability = {
  enabled = true,
  include_session_name = true, -- Default false; may disclose a session title
  -- Keep your existing endpoint and transport settings here.
}
```

Merge these fields into your existing `observability` configuration rather than
replacing transport settings. Names pass through the canonical redactor and are
captured at root launch. Renaming changes subsequent runs, not stored traces;
missing names remain absent. Unsaved runs omit persisted-session fields. Older
binaries/data without exported session attributes remain visible in **All runs**.
Enabling the option does not backfill old telemetry. Do not promote session IDs,
trace IDs or names to Loki indexed labels or metric labels.

### Trace ↔ log navigation

Select the trace ID in any Tempo table to open Explore's waterfall. For
navigation without Explore, copy the trace ID into the dashboard's **Trace ID**
filter and inspect the log panel; reset session filters when inspecting a trace
from the unfiltered table.

Cross-data-source links are data-source configuration, not dashboard JSON.
The inspected local stack already has both mappings below. On another deployment,
ask its administrator to configure them with the selected data-source UIDs:

- Loki derived field: name `trace_id`, matcher type **label**, matcher regex
  `trace_id`, internal Tempo link value `${__value.raw}`. Expand a log row and
  follow the derived field to open the trace.
- Tempo `tracesToLogsV2`: Loki data source, tag mapping `service.name` →
  `service_name`, custom query `{${__tags}} | trace_id = "${__trace.traceId}"`.
  Use the span's logs action to inspect correlated records; widen its search
  time window when investigating lifecycle events beyond that span.

These mappings intentionally filter structured metadata, not raw log body text.
The template does not provision data sources or alter authorization.

## Traces

In Explore choose Tempo and use TraceQL:

```traceql
{ resource.service.name = "capstan" }
```

Open a returned trace ID to see the timeline, then expand an `agent.model` or
`agent.tool` span. Search results can contain only the attributes selected by
the search query (such as `service.name`); they are not the full stored span.
The full trace includes provider/model/profile, turns/attempts, token counts,
HTTP measurements, outcome and tool names where applicable.

A running invocation can show `<root span not yet received>`: native spans are
exported at completion, so finished children can arrive before their root.
Refresh after the invocation completes. This is not by itself evidence of loss.

## Logs

In Explore choose Loki:

```logql
{service_name="capstan"}
```

For one trace, replace the example ID with the selected trace ID:

```logql
{service_name="capstan"} | trace_id = "cfffd20b25525641a66fd56ffc66e3b5"
```

Expand a log row: the body is `span.started` or `span.finished`, while operation,
model/tool, trace/span IDs and measurements are structured metadata. Loki's
indexed label list may show only `service_name`; that does not mean the metadata
is missing. Do not promote trace/session IDs to indexed labels just to display
them.

## Verified delivery and remaining work

On 2026-09-09 the real Tempo trace API returned model/tool children from the
current Capstan invocation with token usage, HTTP phases and byte counters.
Loki query_range returned correlated lifecycle records with structured metadata.
For example, one model span took 4870 ms, with first semantic output at 4328 ms,
HTTP TTFB 2786 ms, upload/server wait 2596 ms and download 2079 ms. Cumulative
milestones overlap: do not add them to phases or interpret download as pure
network transfer time (it includes streamed generation).

### Dashboard validation (2026-09-09)

Read-only checks through `http://127.0.0.1:3000`:

- `/api/health`: HTTP 200, Grafana 13.1.0, database OK.
- `/api/search`, `/api/dashboards/uid/capstan-local`, `/api/datasources`:
  HTTP 200; the existing dashboard is provisioned and this caller cannot edit,
  save or administer it. Tempo/Loki correlation mappings above are present.
- Python `json.load` of the dashboard file, unique panel IDs and default
  variable expansion: passed.
- Exact expanded table queries via Tempo `/api/search`, limit 100: HTTP 200;
  all-runs query returned 4 traces, session-filtered query returned 0. Existing
  inspected run spans lacked session fields. The optional-name predicate was
  separately checked against those unnamed runs: default `.*` returned 4 traces,
  a nonexistent specific name returned 0.
- Exact expanded log-panel query via Loki `/loki/api/v1/query_range`, `since=24h`,
  limit 1,000: HTTP 200, 926 records. Restricting to one returned trace ID yielded
  518 records; an all-zero nonexistent trace ID yielded 0.
- Tempo `/api/traces/<selected-id>`: HTTP 200, 14 resource batches. This proves
  stored trace retrieval and log correlation, not browser navigation.

Those read-only checks preceded the import and fresh acceptance below; their
counts are point-in-time observations, not fixtures. Empty session queries alone
did not establish positive session filtering.

### Latest browser validation (2026-09-09)

The imported `capstan-session-diagnostics` dashboard was opened with Comparison ID
`grafana-acceptance`, Task `synthetic/resource-check`, Replicate ID `r1`. Its
**Benchmark runs** table returned trace `fba07f2ccbfe8be4a2f0e9f47d3098c5`.
Clicking that table's trace link opened the seven-span Tempo waterfall; expanding
`agent.model` showed both `session.id` and `session.name`. The span's log action
opened Loki in split view with the same trace ID. Prior session → concurrent
trace → Loki → Tempo navigation remains valid. This supersedes the older
no-positive-filter/no-browser claims below, not the stated production limits.

Exporter diagnostics now additionally report the latest categorical
`error.category` (DNS/connect/TLS/auth, transport/HTTP fallback, configuration,
protocol or rejection). They remain explicit local pulls, not a backend metric.
The wiki quick-start is `contexts/grafana-capstan.md`; no credentials are stored.

### Historical benchmark navigation API validation

Read-only, unauthenticated GET checks through `http://127.0.0.1:3000`
validated this comparison-navigation addition without importing or modifying a
backend dashboard:

- Dashboard JSON parsing and unique panel IDs passed. Benchmark variables are
  absent from the legacy/session queries and existing log query.
- Exact table TraceQL from the JSON, expanded with default textbox values using
  Grafana's `doublequote` formatting, returned HTTP 200 via
  `/api/datasources/proxy/uid/tempo/api/search` (24 hours, limit 100).
  All runs returned 37 traces; Session runs 17; Benchmark runs 37.
- Each benchmark selector independently set to
  `^capstan-no-such-benchmark-9e163451$`, with the other two at `.*`, returned
  HTTP 200 and zero traces. This checks each predicate and default fallback,
  not positive ingestion or paired-attempt correctness.
- The unchanged default log query returned HTTP 200 and 1,000 records via
  `/api/datasources/proxy/uid/loki/loki/api/v1/query_range`.

Counts are point-in-time observations. Positive filtering of tagged benchmark
attempts and browser interaction with the new table remain unverified; no
browser rendering or new trace/log-link acceptance is claimed by these API
checks. The new table uses the existing native Tempo trace-table navigation.

### Browser validation

Subsequent browser validation reported by the orchestrator imported dashboard
UID `capstan-session-diagnostics`, selected session
`grafana-cli-delay-1788917139827540000`, and opened trace
`a5da4705e57b7a0a5ae60de08d53c077` with its timeline and session attributes.
Removing `select` from the dashboard JSON fixed a mixed-response-frame browser
crash. This supersedes the earlier no-import/no-rendering limitation; it does
not establish anonymous Explore access or every navigation path.

[Acceptance evidence](acceptance.md) records native context/root coverage,
real CLI controlled-delay delivery (5 spans, 10 lifecycle logs), synthetic native
failure/cancellation (3 spans, 6 lifecycle logs), and measured local overhead.
The native baseline is orchestrator-reported; the report distinguishes retained
CLI/failure artifacts from expected native counts. Its API-only browser disclaimer
applies to that report, not the subsequent browser check above.

Final scope: bidirectional browser trace/log links, benchmark filtering, real CLI
concurrent children and stream retry/error, and relay-injected 503 recovery are
verified in the latest evidence above. Shared root totals were additionally
verified in storage; final-binary overhead is recorded in acceptance.md.
Remaining validation limits are live CLI cancellation, complete visual
rename/switch history, actual backend process restart, prolonged outage,
production load and remote TLS/authentication. The short synthetic overhead
measurement is not a production benchmark. Exporter-loss diagnostics remain
explicit local pulls, not dashboard metrics.
