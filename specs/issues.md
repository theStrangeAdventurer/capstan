# Review issues

## Status and ownership

The issue registry and verdict protocol are connected to
[completion review](completion-review.md). The runtime holds the final answer,
provides permission-filtered immutable snapshots and validated transport metadata,
and runs the reviewer and repair/recheck cycle. The registry itself does not
schedule reviewers. Snapshot strings in this module are identities, not file
checksums or proof that a workspace snapshot exists. Active review recovery is
not supported.

`agent/issues.lua` owns identity, revisions, transitions and persistence policy.
`agent/review_verdict.lua` validates untrusted reviewer output. The plugin is a
command/model adapter only; native code stores opaque session JSON in bounded
JSONL chunks. Active-ledger limits belong to the Lua policy, not the archive.
Issues are separate from task plans: executor completion never resolves a review
finding. Disabling completion review or clearing messages does not erase issues.

## Commands and model access

- `/issues`: compact list, independent of tasks, without a model request.
- `/issues <id>`: active evidence, location, run/snapshot, executor response and recheck.
- `/issues history [run-id|issue-id]`: read-only archived reports and terminal
  issues, optionally restricted to one archived run or issue; no model request.
- `/issues accept <id> <reason>`: explicit user risk acceptance, not a clean
  reviewer verdict. Requires a nonblank reason; preserved as `accepted_risk`.
- Model tool `issues`: `read` or `respond`. Responses require current `revision`,
  `id`, `run_id`, `snapshot`, nonblank `response`, and a `status` of
  `pending_verification` or `disputed`. No model operation can delete, resolve,
  publish reports or accept risk. No arbitrary file target is accepted.

The tool is excluded from child collection and denied at dispatch even if a
child receives it in an explicit whitelist. Profile restrictions still apply.
The native APIs and Lua modules are trusted internal interfaces, not an OS
sandbox against trusted Lua or unrestricted shell.

## Report protocol

The reviewer returns a single JSON object, no Markdown fences or trailing prose:

```json
{
  "verdict": "findings",
  "summary": "A failed save is reported as success.",
  "findings": [{
    "severity": "high",
    "description": "Persistence failure is ignored",
    "evidence": "The return value of save() is discarded.",
    "file": "src/example.c",
    "start_line": 10,
    "end_line": 12
  }],
  "checks": []
}
```

Verdicts are `clean`, `findings`, `inconclusive`; summary and both arrays are
required. Findings require severity (`critical/high/medium/low`), description
and evidence. File and inclusive one-based line range are optional for missing
implementation; a supplied range requires a file and both endpoints.
`findings` verdict requires findings; clean/inconclusive require none.
Inconclusive cannot resolve old issues. It preserves them and records a limited
review, never silently becomes clean.

On re-review, a finding may carry an existing open `id` to reconfirm it. A
`checks` entry carries `id`, `status` (`resolved` or `open`) and evidence. No ID
may appear twice across findings/checks. Every unresolved issue in this run must
be accounted for in a conclusive report; unknown, other-run or already-closed
IDs fail. Clean permits only resolved checks. Omissions never silently resolve
issues. Executor responses remain preserved across reviewer updates.

The bounded parser rejects empty/truncated output, duplicate JSON keys, nulls,
noninteger numbers, trailing commas, unknown report fields, malformed shapes,
NULs, oversized fields and excessive nesting. Strict framing precedes the shared
rxi decoder because that decoder intentionally tolerates some ambiguous forms.
It does not reinterpret prose as structured data or execute reviewer content.
Evidence is review data, never additional trusted instructions.

## Internal lifecycle

`begin(run_id, baseline, snapshot, revision)` atomically records a new run and
returns an opaque handle plus ledger. Handles capture the session store and
baseline; duplicate run IDs are rejected. `snapshot(handle, revision, previous,
next)` advances the expected snapshot. `record(handle, revision, snapshot, raw,
transport)` requires explicit `{ok=true, truncated=false}`, validates the whole
report and commits all updates together. Repeated reports for one snapshot,
stale revisions/versions and released/closed handles fail without mutations.
Transport flags must come from the runtime, never from model output.

IDs (`issue-N`) are assigned by the registry, not by new findings. Each issue
retains run, baseline, original snapshot, latest review snapshot, finding,
status, executor response, recheck evidence and user risk reason. Run records
retain report history. `finish` records a terminal reason and releases the
handle only after a successful save. `release` drops only the in-memory handle;
it does not erase history. A closed run record is not itself proof of acceptance.

Active limits: 100 runs, 100 issues, 256 KiB encoded active ledger. Other limits:
32 reports/run, 128 KiB incoming report, nesting depth 16. Identity strings are
bounded (run/issue 64, baseline/snapshot 128 bytes); description 2048;
evidence/response/summary/path 4096.

When a mutation would exceed an active limit, the registry moves the oldest
eligible closed runs, including all reports and terminal issues, into an archive
until the active ledger fits. Runs with open, pending-verification or disputed
issues are pinned, even if the run itself is closed. Resolved and accepted-risk
issues retain their evidence and reasons. No eligible candidate means an explicit
error without discarding history. The report-per-run limit is not bypassed.

The optional `archive={runs:[],issues:[]}` is part of the same stored envelope;
`issues.read()` and mutation results return only the active ledger, while
`issues.history()` returns the archive. Run IDs remain unique across both;
`next_id` remains monotonic. Archived runs cannot be reopened or mutated through
executor tools. Archival does not make an incomplete review successful.

There is no fixed total archive cap: retaining history consumes session disk and
memory proportional to history. Reads validate the full envelope; saves rewrite
it atomically. Archival bounds model-facing active context, not total storage.

## Persistence and adapters

Session JSONL includes optional `issues_json` separately from `tasks_json`.
Small envelopes remain in the header. Larger envelopes use consecutive
`type="issues_chunk"` rows with `issues_json` fragments (at most 256 KiB each)
and a `final` flag before messages. Incomplete chunk streams fail loading; UTF-8
characters are not split on save. The active ledger and archive share one atomic
session save, not independent writes. Save failures roll back memory and preserve
disk state. Malformed existing ledgers are surfaced, never overwritten with an
empty ledger. Old sessions without the field or archive remain compatible.

TUI uses the active Session; CLI uses its already-attached owned Session. Named
sessions persist immediately; unnamed sessions remain process-local. Native
`agent.issues_get()` returns JSON and a session-generation token;
`agent.issues_set(json, token)` rejects stale tokens, NULs and wrong types.
It accepts the full archive envelope; Lua enforces the active 256 KiB limit. Session switch/new/init/shutdown and CLI attach/detach invalidate
old handles, including switching away and back. These tokens are transient,
not serialized or controlled by model arguments.

ACP selects one in-memory store per session, as for tasks. Internal report
handles retain the captured store even if the selected ACP session changes.
Runtime cancellation/disconnect must still terminate its own review; persistence
is not a substitute for lifecycle ownership. Registry text has the same local
privacy properties as conversation history. Display flattens terminal control
bytes and preserves Unicode; persisted evidence is not rewritten for display.

## Verification

- `test/test_issues.lua`: malformed protocols, copies, revisions, duplicates,
  conclusive coverage, inconclusive preservation, rechecks, executor boundaries,
  manual risk acceptance, captured ACP stores, generation change and save errors;
  300 clean and 300 resolved-issue cycles, reload, archived duplicates, count/byte
  pressure, pinned unresolved states and mutation-free overflow/save failures.
- `make test`: optional-header compatibility, multi-chunk Unicode round trip,
  malformed/truncated archive streams, malformed metadata, failed-write
  preservation and allocation ownership.
- `make test-http-lua`: native generation/bounds, session-manager rollback and
  switching, model tool collection/dispatch rejection for subagents, existing
  task persistence regression tests and standalone issue policy tests.
- `make test-build`: embedded `/issues` and ACP/build smoke regressions.

These registry tests validate policy and storage. Controller and real-binary
review/repair tests are described in [completion review](completion-review.md).
