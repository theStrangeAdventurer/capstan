# TUI presentation

## Visual hierarchy

Assistant prose uses the terminal's normal foreground, not blanket dimming.
Tool headers and shell bodies use dimmed terminal-default foreground, not a
fixed gray/white palette. Task borders, controls and session IDs are
neutral; bold identifies emphasized values. User messages and the session overlay
use the terminal's default background. Each visible user text row (including
wrapped continuations) has a quiet `│` in the existing left padding: gray 245
on 256-color terminals, dim default foreground otherwise. Assistant rows have
no gutter marker. This renderer-only decoration does not enter copied text,
model context, logs or persistence, or change wrapping/selection coordinates.
Purple marks status dots and start-screen profile/shortcut keys; error,
warning and diff colors retain their semantic roles. Technical
headers have no automatic italic styling; Markdown emphasis still does.
Queue previews use this same neutral secondary color, not warning yellow.
Input corners match the rounded task panel corners.
Tool headers, phase captions and shell command prefixes share the same left
edge as result bodies, without synthetic two-space indentation. Result content
(including code and diffs) retains its original whitespace.

The bottom row has one current activity on the left and profile/model metadata
on the right. Activity is normal foreground with a purple pulsing indicator,
never red or italic; an explicit runtime activity retains its elapsed seconds.
Structured tool-call streaming shows `Tool calling` from the first nonempty
call fragment, before execution selects its tool-specific activity. Repeated
fragments keep the same timer. Text/reasoning transitions, completion and transport
errors clear this stream activity; suppressed subagent streams never change it.
Stream regression tests cover these transitions and suppression.
Without an explicit activity, HTTP work falls back to Thinking, Answering or Connecting. No
semantic summary is inferred from tool output. Metadata yields to activity:
drop effort first, then model, then profile; never overwrite activity. Clipping
is Unicode-cell-aware and removes ASCII controls. Model namespace/provider is
omitted here; full configuration remains available through /info and the start
screen. All these changes are presentation-only, not provider policy.

Unknown context limits use explicit `tok in N out N` labels instead of an
ambiguous ratio. Known context limits keep their existing usage/limit percentage.
Workspace statistics say `Changes: N files · +N −N` and describe the whole
workspace, not just the active session. They retain existing narrow-screen
fallbacks and do not consume another row. Their label and separator use muted
secondary text; addition/deletion counts retain their semantic colors. Bottom-row
metadata and the input mode label also use secondary text (gray 245 or dim
fallback), with the active profile bold purple, matching the start screen.
`TuiStatusRow.profile_width` identifies the sanitized profile span for coloring;
absent or hidden profiles do not color adjacent model metadata.

The input border stays quiet and uninterrupted, without a decorative prompt
marker. Input uses the native terminal cursor through ncurses; its color, shape
and blinking follow the terminal's behavior/settings. Capstan does not emulate
blinking/fading with redraw timers or override cursor styling with terminal-specific
escape sequences. Cursor visibility still follows the existing focus handling.
These are renderer-only changes; input contents, wrapping/cursor offsets, model
context, logs, persisted state, CLI output and plugin APIs are unchanged.
PTY palette tests cover the absence of the marker, native cursor visibility,
mode label, profile color in both locations and removal of the unconditional
start-screen `ready` line.

## Existing disclosure mechanisms

Successful tagged shell bodies fold regardless of logical line count, including
long single-line JSON. Errors and unknown status remain expanded. Existing
mouse controls, source mapping, raw model data, logs and persistence remain
intact. See [Shell plugin](shell-plugin.md).

Completed/cancelled task plans automatically collapse unless the user explicitly
chose a view. Blocked plans remain unfinished. See [Task plans](tasks.md).

## Boundaries

This iteration does not add generated tool summaries, hide full shell commands,
add generic folding to other tools, or change queue editing/cancellation and task
viewport navigation. Historical tool phase captions remain historical text;
only the bottom row represents the live activity. These are follow-up work, not
claimed functionality.

## Tests

`make test`: shell disclosure/mapping/error defaults, status-row width/Unicode/
control sanitization, workspace labels and usage formatting.
`vendor/lua-5.5.0/src/lua test/test_tasks.lua`: automatic closed-plan defaults.
`make test-tui-input`: real ncurses colors, Markdown emphasis, shell mouse
controls, task view persistence, reasoning metadata and draft preservation.
`make test-http-lua`: shell projection integration and task/runtime adapters.

## Completion review status

Opt-in [completion review](completion-review.md) uses a separate compact upper
purple indicator: queue, review, repairs and rechecks (with cycle counts).
It is not the lower Thinking/Answering activity label. Runtime owns the label
through `agent.set_review_status`; nil/empty clears it on terminal cleanup.
Held final drafts are not shown as accepted while review is pending; ordinary
tool commentary remains visible. During queued/running review the foreground slot
is free: new prompts and writes run without waiting for the reviewer. Review
results are anchored to their original message; newer requests supersede automatic
repairs and acceptance of old drafts. Status labels are Queued, Review, Fixing and
Rechecking. See the completion-review lifecycle for CLI/ACP differences.
