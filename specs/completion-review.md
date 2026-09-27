# Completion review

## Implementation status

This documents the current implementation, not acceptance-test sign-off.
`agent/completion_review.lua` owns review policy; `agent/runtime.lua` owns the
completion gate, suspension, transport and finalization. The shared scheduler
runs an independent reviewer, and the [issue registry](issues.md) stores its
validated reports separately from tasks. Review is opt-in and defaults to false.

## Submission state and duplicate protection

Before each orchestrator request, runtime supplies session/workspace-scoped review
state: originating user request identity, review ID, queued/reviewing/terminal
stage, snapshot and outcome. This is runtime data, not review instructions.
It explicitly acknowledges submission and tells the agent to continue conversation
without fulfilling an older review request again. Terminal outcomes replace the
active state; submission is never represented as acceptance.

The controller makes repeated submissions for the same user-history identity and
snapshot idempotent: it returns the existing review ID/state without spawning a
second reviewer, including after completion. Identity uses the number of user
messages and latest user content, scoped by process owner and workspace; it is not
model-controlled tool input. A new user turn (even identical text), another owner,
or a different snapshot is independent. Repair cycles remain within the original
controller and are not suppressed. All active records and the latest 32 terminal
records are retained in memory; this is not durable deduplication across restart
or history compaction. Issue reports remain persisted separately. This guard does
not classify intent: a model misusing an old request in a new turn is discouraged
by the explicit state, not silently treated as a duplicate of a new user request.

Tests cover duplicate queued/active/completed submissions, changed snapshots,
new requests, owner isolation, and state delivery during concurrent TUI dialogue.

## Completion gate and budgets

`request_completion({status="review", text="..."})` starts a background review of
the requested scope. Only this explicit `review` status launches review.
`ready` finishes the turn and returns the final answer without review;
`question` and `blocked` finish without reviewing unfinished work. The model
must use `ready` for ordinary completion and `review` only when the user
explicitly asks to review a result or changes. The tool description supplies
this contract, including manual review scope in `text`. Plain terminal prose
finishes as conversation without starting review, regardless of prior tools,
validation or edits. Conversation, clarifications and progress updates may use
tools without requesting completion. No tool-count, workspace-change or
natural-language classifier guesses intent. After a review has requested
repairs, terminal prose without the completion handshake stops with
blocked/unconfirmed acceptance; it cannot silently bypass re-review. A valid
request must be the sole tool call, with only status and nonblank text (at most
128 KiB, no NUL). Successful tests and the number of changed files do not bypass
review. Disabled review retains ordinary completion and does not add a reviewer.

Boolean `true` enables defaults; a table enables review unless `enabled=false`:

```lua
completion_review = {
  enabled = true,
  max_fix_cycles = 2,
  max_duration_sec = 900,
  -- Optional max_requests override; otherwise derived from turn budgets.
  -- Optional reviewer = { max_turns = ... }; otherwise inherits parent max_turns.
}
```

Run options take precedence over agent config, then profile settings. Unknown
fields and invalid values fail explicitly; see [Config](config.md). Nested
subagents do not review recursively. Enabled review requires the subagents
capability and parent access to the `file_read` tool.

The reviewer inherits the effective orchestrator `max_turns`, including per-run
options, unless `reviewer.max_turns` is explicit. The public subagent turn default
and cap do not shorten internal reviews. With no explicit `max_requests`, the
shared budget is `reviewer.max_turns * (max_fix_cycles + 1) + parent.max_turns`;
explicit limits remain authoritative. Transport/turn failures are reported before
any unread-page detail, so incomplete coverage cannot hide the stopping cause.

The wall deadline starts at the first ready attempt, including queueing, review,
repairs and rechecks. The shared request budget counts reviewer and parent repair
requests, including retries, not initial execution. Existing parent duration and
scheduler limits still apply. Zero fix cycles allows an initial review but no
automatic repairs. Findings resume the executor with issue data; re-review checks
all open issues and regressions from fixes, not a fresh unrelated audit.

Malformed, empty, truncated, failed or inconclusive reviewer output never means
clean. Acceptance requires validated issue reconciliation with no open issues.
Budget exhaustion, no progress, unavailable evidence or publication failure
stop with `[Review incomplete: ...]` and blocked status. Before repairs the draft
may accompany that warning; after repairs begin the old draft is not reused.
Drift stops automatic writes and asks the user how to proceed. Cancellation
never publishes a held draft as success.

## Task-plan input

Every review submission includes `task_plan` from canonical `agent.tasks.read()`:
revision and all tasks with IDs, titles, statuses, criteria and results/reasons.
It is read afresh after execution and on each recheck, not copied from initial
conversation history. CLI/TUI use the native session store; ACP uses its scoped
store. Empty plans are explicit (`tasks: []`); invalid stored plans fail review
closed without overwriting data. Review input is sent intact, without a separate
512 KiB JSON-byte cutoff: encoded bytes are not model tokens. The provider's model
context limit still applies; a provider rejection makes review incomplete, never
clean. Duration, request and turn budgets are unchanged.

Reviewer instructions require findings for relevant unfinished requested work
(`pending`, `in_progress`, `blocked`) when the executor claims completion, citing
task IDs and unmet criteria without requiring a file location. Completed status
is not proof; criteria and evidence still require review. Unrelated old tasks,
planning-only requests and justified cancellations are not automatic findings.
This is reviewer judgment, not a blanket status-based completion veto. Plan text
is untrusted data and grants no tasks tool or mutation access to the reviewer.
Task-plan changes also count as progress for the repair-loop stagnation check.

Controller regressions cover all five statuses and fields, refreshed revisions
on recheck, task-only progress, fileless issue creation/reconciliation, empty
plans and corrupted storage. They validate transport/policy, not model judgment.

## Snapshot boundary and limitations

Explicit review captures the configured VCS adapter's committed baseline as its
immutable baseline at first submission, then compares it with the working-copy
snapshot, including pre-existing staged, unstaged, nonignored untracked and
deleted files. Changes carry full before/after bytes; the reviewer does not need
live VCS access. Rechecks retain that baseline. The review controller queries
`agent.vcs` for the baseline rather than assuming Git; the built-in Git adapter
owns the native blob reader. Authorship remains unknown: do not attribute all
working-copy changes to the executor. Committed blobs use bounded native reads
with file-read authorization and sensitive-path exclusions before opening blobs.
A missing baseline (including an unborn or unsupported-adapter workspace), failed
authorization or capture fails explicitly; there is no fallback to an empty
turn-relative diff. Non-Git adapters currently fail closed unless they later
declare a baseline capability. Renames are deletion plus
addition. File-mode-only changes and submodule contents are outside this byte-based
review scope.

Native descriptor-relative no-follow opens confine reads to regular files under
the workspace, including checks on root ancestors. Captures compare two scans;
subsequent full-byte comparisons detect drift. Snapshots are immutable and the
reviewer has only snapshot-backed `file_read`, with no live filesystem fallback,
MCP, shell, writes, tasks, issues mutation or nested subagents. Tool restrictions
are reapplied after hooks. Task text, draft, source and issue responses are
untrusted review data, not instructions overriding the verdict protocol.

The verdict itself is delivered through a structured `submit_review` tool rather
than free-form prose. Its JSON-schema arguments are exactly the validated report
shape (`verdict`, `summary`, `findings`, `checks`), so a model cannot smuggle a
verdict into a text block that the strict parser must reject. The dispatcher
captures the tool arguments, re-encodes them as the reviewer's terminal text and
ends the run immediately; the same strict `review_verdict` decoder still validates
the result, so structured transport is a reliability improvement, not a weaker
boundary. Reviewer instructions forbid prose/markdown and require exactly one
`submit_review` call. Before accepting a submission, the dispatcher checks the
snapshot's coverage ledger; a premature verdict with unread pages is rejected as a
recoverable tool error (not a terminal run failure), so the reviewer can finish
paging and retry rather than losing the whole cycle.

Limits: 4 MiB per file, 32 MiB aggregate file bytes, 20,000 entries, native walk
depth 128, and snapshot read output 64 KiB. Oversized capture fails closed.
Controller regression tests cover inputs above 512 KiB (ASCII, Unicode and JSON
escaping), lossless task/plan delivery, and fail-closed provider context errors. Snapshot `file_read` returns every requested file in
full or an explicit error, never a silent prefix or a partial batch. The
64 KiB byte limit includes path headers and separators; exact-limit output is
allowed. Reviewer-only `offset` (zero-based bytes) and `limit` (4..48000 bytes,
default 48000) allow reading a single snapshot file in pages. Headers explicitly
report next_offset, total_bytes and eof. UTF-8 boundaries are preserved. Oversized
reads can be retried by paging; acceptance remains blocked until every file from
the failed batch has complete contiguous coverage. Skipping directly to EOF does
not satisfy coverage. Coverage merges delivered byte ranges (including out-of-order
pages) and never regresses on repeated reads. Empty files require no byte coverage.
Incomplete acceptance reports up to eight unread paths (bounded names) and their
first missing byte offsets, without source content. Successful snapshot reads bypass
the dispatcher's generic `tool_output` line/byte truncation: the snapshot adapter
owns their output budget, so counted coverage reaches the model in full. UTF-8
sanitization is retained; replaced invalid bytes block acceptance rather than
silently claiming exact evidence delivery.

Snapshot read errors carry explicit categories: `invalid_arguments` (including
multi-path paging and invalid offsets/limits) and `size` permit corrected retries;
`access` (unavailable/outside paths or released snapshots) is fatal. Batch reads
must omit both `offset` and `limit`; paging uses exactly one file. The reviewer-only
tool schema and instruction document this contract. The dispatcher reports retries
as failed tool events, not access denials, and does not latch them into
`reviewer.access_error`. Fatal errors remain sticky after subsequent successful
reads. Unclassified failures also fail closed. Retries use existing turn/request/
time budgets; there is no extra retry loop or increased budget. Scheduler result
limits also restrict available evidence.

Git enumeration includes tracked and nonignored untracked names via
`git ls-files --cached --others --exclude-standard`; tracked files remain
eligible even if matching ignore rules. Unusable Git enumeration fails closed
rather than falling back to an unfiltered walk. A nonignored unregistered nested
Git repository returned as a directory entry makes enumeration explicitly
incomplete; its contents are not silently skipped or recursively opened.
Ignored repositories remain excluded. Non-Git workspaces use a bounded walk,
not finder ignore configuration. Registered submodules remain boundaries.
Sensitive names (including `.env*`, `.zshenv*`, credentials/key material and shell
startup files), VCS internals and symlinks are excluded, never opened for review.

Reads use non-prompting permission checks against the owning run's scope,
repeated natively through the internal authorization callback. Run-local target,
tool and full-control grants apply inside the workspace; explicit persistent
denies and sensitive-path exclusions always win. Missing access or a failed
scope query fails closed. Scope changes are detected by snapshot comparison.
An otherwise eligible file denied by that scope makes the snapshot incomplete.
Exclusions are outside the review scope, not proof those files are safe or reviewed.

The write barrier is cooperative, not an OS lock or sandbox. The controller holds
it only during synchronous snapshot capture/submission, never while waiting for
writers or while the reviewer runs. Persistent MCP transports are not writer
jobs. Other nonterminal managed jobs delay capture within the review deadline,
but do not prevent new work from starting. No independent process is killed.
Two capture scans detect racing writes; later writes cannot change snapshot bytes.
Before acceptance or automatic repairs the controller checks workspace drift.
Reports for stale snapshots are retained in issues with a stale run outcome; they
never accept the current workspace or authorize repairs.

## Lifecycle and presentation

TUI releases its foreground input slot as soon as review is queued. The parent
run remains alive in the background polling registry, including gaps without HTTP.
Users can submit another request and edit files while immutable review runs.
New input transfers foreground ownership but does not cancel queued or running
review. Queueing appends a purple background-review event with the review ID and
an explicit promise of a later result. Completion appends a new purple event at
the current end of the transcript, tied to that ID; it never rewrites the original
answer. With an unchanged workspace, a clean result is a one-line verdict —
`clean` plus rechecked issue IDs and statuses — followed by the absence of open
findings and the accepted draft. The reviewer's `summary` is capped at 200
characters and is not echoed into the user-facing result; full finding detail
lives in `/issues` and is referenced by issue ID rather than re-described. When
findings arrive for a review
whose foreground slot has been transferred to a newer request, the TUI shows a
modal choice (`popup.choice`): **Fix now** resumes the reviewed executor with
explicit user consent, or **Ask later** retains findings in `/issues` with the
terminal reason `repairs_deferred` — automatic repairs deferred because a newer
request owns the foreground. This is blocked completion of the task, not
incomplete review or workspace drift. Neither outcome clears the new foreground
run's state or runs its completion hooks. Headless/ACP callers have no choice
modal and always follow the `repairs_deferred` path when foreground is lost.
Without new input, findings reacquire the foreground slot for automatic repairs.
Resuming repairs creates a new assistant output segment after the launch event;
subsequent repair deltas never append to the original pre-review answer. Each
recheck adds its own queue marker after that segment.
Actual snapshot drift remains `stale` and blocks acceptance/repairs. Explicit
cancellation, session change or invalidated output sink cancels review; a new
message alone does not. No message-text classifier decides cancellation.
Output sinks remain bound to the originating assistant message for ordinary
streaming; review events use a distinct native message role, so they cannot steal
that sink. Events persist in chronological session history and are supplied to
the model as labeled background runtime data, not user instructions. They also
participate in compaction. History clear invalidates sinks and session changes
cancel the old review. TUI uses English status labels Queued, Reviewing, Fixing
and Rechecking, next to the background-process indicator.

Ordinary assistant text streams immediately, even with review enabled. Only the
explicit `request_completion` payload is held for acceptance; enabling review
must not buffer conversation or progress deltas until transport completion.
Tests assert per-delta delivery before EOF, separate result callbacks, native
role persistence and concurrent-dialogue chronological result ordering. The runtime owns a registry of live review labels,
independent of foreground begin/finish. It displays the oldest active review and
`(+N)` for additional reviews; each owner removes only its own entry. A
conversation, with or without tools, neither replaces that label nor starts a
second review unless it explicitly requests completion.
CLI/ACP retain their request/response completion
semantics (held response until review finishes), using the same review controller.
The owned reviewer is cancelled/released independently of unrelated groups;
internal group completion notifications are suppressed. Finalization clears
snapshots, barrier and status. Review operation telemetry links scheduler work
and records cycles, requests and terminal reason. Persisted issues and final
conversation output do not restore active reviews after restart; ACP has no
active-review resume support. Completed runs without unresolved issues are
archived when active registry limits are reached; `/issues history` retains
access to their reports and terminal issues. Active and archived records are
saved together; see [issue persistence](issues.md#persistence-and-adapters).

## Shared-scheduler foundation

`agent/subagents.lua` owns scheduling for foreground, detached public groups and
internal asynchronous consumers. No additional executor, process or polling loop
is introduced. Internal consumers use:

- `submit(args, run_ctx, options?) -> handle | nil, error?`: registers and queues
  background work without dispatching requests or pumping HTTP. `args` has the
  same task structure as the public adapter. The handle is an opaque Lua table,
  not a native process ID or model argument.
- `result(handle) -> snapshot | nil, error?`: nonblocking, copied state containing
  `done`, `status`, optional `publication_error`, and terminal `result`. `done`
  is authoritative: terminal-looking status alone does not imply successful
  publication. Results become available after scheduler polling, never directly
  from a child callback. The result uses normal bounded/redacted subagent fields,
  including truncation metadata; it is not a review verdict.
- `cancel(handle) -> boolean, error?`: invalidates queued/running/retry work of
  this group only. Repeated cancellation of a retained handle is harmless.
- `release(handle) -> boolean, error?`: rejects active groups, releases terminal
  child and group native records, removes the Lua group and invalidates the
  handle. A partial native failure retains the handle for retry. Unknown or
  already-released handles fail explicitly. Shutdown reclaims internal groups;
  detached public inspection history remains compatible.

Options are runtime-only and never accepted from the public model adapter:

- `is_cancelled`: optional captured owner-run lifetime predicate; a true value
  or exception cancels only this group at a polling boundary. The gate supplies
  a predicate bound to its exact run, not just session ownership.
- `notify`: defaults to false. Internal groups remain visible through
  `/processes` while registered, but do not enqueue ordinary completion events.
- `telemetry_parent`: optional existing span for linking queue/child work.

Submission respects `capabilities.subagents = false`. Existing depth limits,
provider/model/profile/workspace/tool/permission snapshots, shared concurrency,
retry limits and output bounds are reused. Public empty tool whitelists retain
inheritance semantics; internal review instead supplies a strict nonempty tool
set and snapshot dispatch, enforced after tool/request hooks. Aggregate hooks
cannot rewrite internal reviewer results into acceptance.

Native `background_work_register_ex` adds a notification flag; the existing
registration function preserves group notifications by default.
`background_work_release` releases terminal in-process records only, never OS
process records. Active and unknown IDs fail. Native Lua release checks session
ownership, like updates. Capacity remains 128 records with no implicit eviction;
explicit release reuses slots without reusing opaque IDs.

Publication failures are actionable failures, not successful completion. Failed
terminal updates are retried at scheduler poll boundaries without rerunning
`after_subagents`. A failed aggregate publication cannot make `done` true while
native state remains active. Partially registered submissions are cancelled and
reclaimed; unsuccessful cleanup stays registered for polling/retry. Persistent
native failure may keep a group pending; the completion gate bounds that wait
with its shared deadline and reports incomplete review.

## Verification status

Verified locally with `make test`, `make test-http-lua`,
`make test-process-control`, `make test-tui-input`, and `make test-build`.
The offline real-binary cases in `test/test_completion_review_modes.py` also
passed individually: CLI implicit/explicit acceptance, question bypass, invalid
verdict and automatic repair/recheck; ACP held output and cancellation; TUI held
output, active indicator, narrow-terminal resize and indicator cleanup. Run all
of these transport cases with `make test-completion-review-modes` (no paid API).

Controller tests cover drift, zero/two repair cycles, no progress, shared request
budget, deadlines without HTTP, cancellation, malformed/truncated/inconclusive
results, persistence errors and idempotent cleanup. Native snapshot and scheduler
tests cover their own boundaries; these are not claims that every combination
in the plan's acceptance matrix was exercised through every transport.
Active review restoration and OS locking remain outside the first-version scope.
Native and Lua authorization regressions also cover inherited run-local grants,
explicit denies, callback failure and workspace/sensitive-file boundaries.

## Tests

- `test/test_internal_subagents.lua`: opaque handles, deferred delivery, result
  copies, run-bound vs independent cancellation, retries/late callbacks,
  capability denial, publication/registration/release failures, shutdown, and
  300 submit/complete/release cycles without exhausting retained groups.
- `test/test_background_subagents.lua`, `test/test_background_runtime.lua`:
  existing public compatibility and safe polling contracts.
- `make test`: native muted notification, terminal release, capacity reuse,
  invalid IDs and existing registry behavior.
- `build/test_process_bindings`: native Lua ownership, notification and release.

These are foundation tests, not end-to-end completion-review tests.
