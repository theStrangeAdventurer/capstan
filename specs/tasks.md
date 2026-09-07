# Persistent task plans

## Behavior

A task plan is a session artifact, not a checklist inferred from chat history.
The built-in `tasks` model tool supports `read` and `update`. The Plan profile
natively creates/refines plans; Implement creates one only when the user requests
planning. With an existing plan, Implement consults it, records meaningful status
transitions, and accounts for relevant unfinished work before finalizing. These
are agent instructions, not proof of completion or a forced extra review turn.
An unrelated request does not authorize executing old tasks. Creating a plan does
not approve its execution.

`/tasks` displays the current plan without an LLM call or adding model history.
Nonempty plans expand by default into a framed block directly above input,
with rounded top corners, the same outer width and horizontal inset, and the
terminal's default background (no hardcoded black fill). The panel border and
its toggle use the logo's purple (palette index 141, magenta on 8-color terminals). The input's top edge
closes the panel. Its full height is deducted from the message viewport before
rendering and scroll calculations; it never overlays message history. Status marks
(`○`, `◐`, `✓`, `!`, `−`) precede titles; wrapped continuation rows align with
the title, not the status. ASCII control characters (U+0000–U+001F and
U+007F) are rendered as spaces using explicit byte ranges, never locale-sensitive
Lua character classes. UTF-8 titles remain intact; display cleanup does not
modify stored titles. Lua tests assert exact Cyrillic/mixed Unicode text under
a UTF-8 locale; PTY tests check intact Cyrillic titles, wrapping and resize.
A padded `[ Tasks completed/total ⌄ ]` control is centered in the panel's top
border. Collapsed input shows the same centered control with an upward chevron
on its top border, rather than beside INSERT. The whole control, including its
brackets and padding, is clickable. Ctrl+G toggles expansion using the plain
BEL control byte (no modified-arrow encoding or terminal configuration needed).
With no tasks it is a no-op; popups retain priority. Ctrl+Shift+Up/Down remain
legacy collapse/expand aliases when the terminal forwards them.
On narrow terminals the label is shortened symmetrically
without overlapping mode or usage; if no room remains it is hidden (keys still work). Shift+Up/Down
still adjusts reasoning effort; draft text is preserved, including during waits.
Popups retain input priority and cannot toggle the underlying task view.

The viewport uses at most nine rows and approximately half the available chat
space, preserving at least one chat row. Wheel scrolling inside the block and
Ctrl+Shift+PageUp/PageDown reach overflow rows. Resizing clamps the row offset;
very small terminals fall back to the collapsed control, clipping the summary
without displacing usage. Empty plans do not reserve space.

`tasks.expanded_by_default = true` in unified config controls the initial view.
Manual choices override config per session and persist as optional `tasks_view`
metadata (0: config, 1: collapsed, 2: expanded), independently of the task plan.
Legacy or invalid view values use config. Save failures preserve the old view
and show an error. Scroll offsets are transient and reset on session reload.
Users can ask the agent to revise the plan. There is no background reviewer in
this version and `completed` means executor-finished, never independently reviewed.

Each record has a stable ASCII `id`, `title`, `status`, optional acceptance
`criteria`, and `result`. Statuses: `pending`, `in_progress`, `completed`,
`blocked`, `cancelled`. The latter three require a nonblank result/check summary
or reason. Updates supply the current revision and complete records to upsert;
omitted IDs are retained. Cancel abandoned tasks rather than deleting them.
Reading returns the complete current revision. Stale revisions, duplicate IDs,
invalid fields and excessive size are errors with no state changes.

## Ownership and paths

- `agent/tasks.lua` owns validation, revisions, upsert semantics, display and
  context generation. Maximum 100 tasks, 128 KiB encoded JSON; IDs 64 bytes,
  titles 512 bytes, criteria/results 2048 bytes each. Strings reject NUL.
- `plugins/tasks.lua` exposes the tool and `/tasks`. Its permission is `false`:
  the tool can change only bounded session metadata, never a chosen file path.
  Plan explicitly permits this exception without permitting project writes.
- C stores opaque task JSON in the optional `tasks_json` header string in session
  JSONL, using the existing atomic save. Old sessions have no plan. C enforces
  the size bound, owns the string, and transfers/frees it with its session.
- `session_manager_set_tasks` saves immediately, even with no message changes.
  Save failures roll back task data; Lua reports failure, not success. Existing
  invalid task JSON is surfaced and not silently overwritten.
- TUI session switch, new session and restart use the same Session ownership.
  Compact/clearing conversation messages does not clear the task artifact.
- CLI attaches its owned Session to the same adapter. Named runs save every
  update; unnamed runs retain plans only for the lifetime of that process.
- ACP selects a separate in-memory store for each serialized session prompt.
  Plans live as long as ACP sessions (ACP does not support session resume).
  Successful tool calls emit ACP `plan` updates; blocked/cancelled map to pending
  with reasons in content because ACP only has three plan statuses.
- Before every top-level model request, including tool continuations, runtime
  injects one fresh task-plan message separate from conversation history. Empty
  plans add no context. Stored criteria/results are data, not instructions.
- Subagents do not receive this injected message, do not collect the task tool,
  and cannot dispatch it even if explicitly whitelisted. Parents coordinate work
  and update the plan from child findings.
- Existing tool logging/results apply; task content has the same local storage
  privacy properties as conversation content. No project TODO file, global plan,
  automatic reviewer, background request or new configuration path is added.

## Tests

`make test`: session serialization, view preference round trips, transient scroll,
legacy empty state, bounds, viewport sizing/clamping and preservation.
`make test-tui-input`: real ncurses task keyboard/mouse toggles, overflow,
configuration defaults, nested waits and draft preservation.
`make test-http-lua`: immediate saves, rollback, CLI adapter, session switching,
message clearing, Lua binding bounds, pure Lua task policy, per-store isolation,
invalid updates, compact-style context replacement, actual provider request
injection/refresh, Plan tool execution and subagent rejection.
`make test-build`: embedded module/plugin availability and binary build.
`make test-acp`: ACP protocol regression smoke test.
