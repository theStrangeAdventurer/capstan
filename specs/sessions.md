# Sessions

Capstan persists workspace-scoped TUI conversations and explicitly named
headless runs.

## Behavior

- TUI startup restores the last active session for `app_workspace_root()`.
- If no active session exists, Capstan creates an empty one.
- `/new` saves the current conversation and creates a new empty session.
- `/sessions` opens a filterable list sorted by most recent update. The active
  session is marked with `*`; Enter switches to the selected conversation.
- Switching clears transient input, queued submissions, buffered plugin results,
  scroll state and message cursor state. Token usage is restored from the selected
  session; new sessions start at zero. Those values never cross session boundaries.
- Sessions persist the last displayed prompt/completion/total token counts and
  context limit as a `type: "usage"` JSONL row. These are the latest request's
  statistics (or streaming estimate), not cumulative billing. Usage-only changes
  trigger the same debounced save and shutdown/switch flush as message changes.
  Headless named runs also save their final displayed usage. Old sessions without
  usage remain readable with zero counters until the next request; no historical
  API statistics are fabricated. Malformed or out-of-range counters fail closed.
  The saved context limit is a display snapshot, not a configuration override;
  the next request resolves its limit from current model/provider configuration.
  Tests cover disk round trips, usage-only shutdown saves, restart, and switching.
- Switching commits the durable `active` pointer before replacing the live
  session. If that atomic write fails, the loaded candidate is discarded and
  both the current messages and in-memory active session remain unchanged.
- Commands, including session switching, remain unavailable while a top-level
  agent run is active, following the queued-input command policy.
- After the first successful assistant response, Capstan asks the effective
  active model for a concise 3–7 word title in the user's language, honoring
  profile selections and interactive launch provider/model/reasoning overrides.
  Until that request succeeds,
  the UTF-8-safe beginning of the first user message is used as a local fallback.
  Generation runs as background HTTP work without tools or a loading spinner,
  does not enter conversation history or append to the visible assistant
  response, does not invoke `after_agent_turn` hooks, and is not canceled by
  stopping a user-visible response. It does not replace the title if the user
  has switched sessions before it completes. At most one title request may be
  in flight for a session.
- Empty assistant streaming placeholders are not persisted.
- `--session-id "release prep"` selects that workspace session in either mode.
  If it exists, Capstan restores its complete visible/model history. If it does
  not exist, Capstan creates it with a stable ID and explicit title exactly
  matching the supplied value, then makes it active. Background title
  generation never replaces an explicit title.
- Headless mode persists the new prompt before provider work, sends the complete
  stored history (including user images), and persists non-empty assistant
  output when the run finishes. Invalid IDs, malformed existing session files,
  and storage errors fail closed without overwriting history.
- Headless runs without `--session-id` remain one-shot and do not read or write
  sessions.

## Session header

The TUI paints the active title and ID in a top-right overlay, without reserving
chat rows. Its two lines have a user-message background, a dim border and an
bottom-only shadow and two cells of left inner padding. The ID line uses
`session.id: <id>` (the exact OpenTelemetry attribute name); the value and copy
mark use brand purple (141, magenta fallback) on the overlay background, while
the label stays neutral. Copying still delivers only the raw ID. The `[x]` button closes it; embedded `/show-session` opens it
without arguments, adding history or calling the model. The command is available
in slash-command autocomplete. It starts hidden on each application
launch; visibility is transient application state, unchanged by session switches. Each row ends in an overlapping-squares copy mark (`⧉`, ASCII `[]`
in non-Unicode locales). Clicking either the text or mark copies the complete
raw value, not its display truncation. Successful copies blink with the existing
70 ms selection-copy rhythm and show `Copied`; clipboard failures show an error
without a success blink. Message selection and header actions share the native
clipboard writer (`pbcopy`, or `wl-copy`/`xclip`/`xsel`).

Rendering reads the session manager on each frame, so switching, `/new`, and
background title completion cannot leave a cached title or ID. Display text is
cell-bounded, preserves UTF-8, and flattens terminal controls. The header is
hidden below 11 columns or 9 rows rather than overlapping input. Chat, shell
controls, queued input, and mouse selection use the full conversation viewport.
The overlay is painted above chat and below modal popups; closing restores chat.
`make test` covers narrow/Unicode layout; `make test-tui-input` covers actual
clipboard-command delivery, `/new`, resize, and shell mouse geometry.

## Review metadata

Optional `issues_json` stores the [review issue registry](issues.md) independently
of tasks and conversation messages. Lua caps the active ledger at 256 KiB and
archives eligible closed runs without deleting history. The complete envelope
uses a header field or bounded `issues_chunk` rows and follows the same
atomic-save/rollback and owned-session lifetime rules for active state and archive. Native generation tokens
reject late report writes after session switching, new-session creation or CLI
detachment. ACP has a separate in-memory issue store per session; this does not
add ACP resume or restore active review execution.

## Storage

Sessions are stored under the XDG state directory:

```text
$XDG_STATE_HOME/capstan/sessions/<workspace-hash>/
  active
  <session-id>.jsonl
```

The fallback is `~/.local/state/capstan/sessions/...`.

The first JSONL row contains versioned metadata. Later rows contain `role`,
`text`, and `raw_text`; preserving both keeps the visible representation and the
model context distinct. Files are replaced atomically through a temporary file
and `rename()`.

Explicit shell-output ranges are stored as `type: "shell_output"` rows following
 their message, with zero-based byte offsets `start` (inclusive) and `end`
(exclusive) into `text`. Loading validates ordered, non-overlapping, non-empty
ranges within the message and rebuilds display metadata using `shell_output_add`.
Long outputs reopen collapsed with clickable `[+]` controls and shell styling;
expansion state and projected display text are never saved. Legacy messages
without ranges stay readable as plain text; shell-looking text is not guessed.
Malformed range rows fail closed. Tests cover range validation and a session
manager round trip from expanded output to a collapsed, clickable restored block.

Session directories use Unix mode `0700`: only the owner can list, modify, or
enter them. Session and `active` files use `0600`: only the owner can read or
write them.

Invalid identifiers are rejected. Unsupported or malformed session files are
ignored by listing and fail closed when explicitly loaded; they do not crash TUI
startup. Reads cap individual JSONL rows at 4 MiB and sessions at 10,000
messages. A row exceeding the byte cap, an allocation failure, or an underlying
read error invalidates the whole load and is never treated as a clean EOF.
Before extracting fields, the loader validates every non-empty row as one
complete JSON object, including metadata, messages, and image chunks. Missing
closing delimiters, malformed numbers (such as `1garbage`), invalid escapes,
raw control bytes/NUL, invalid UTF-8, and trailing non-whitespace are rejected.
Validation bounds nesting to 64 levels. Fields are read only from the top-level
object, with JSON whitespace allowed around separators; nested lookalike keys
cannot supply required fields. Integer fields reject fractional/exponent tokens
and overflow rather than silently truncating them. A failed load clears all partial state;
load-or-create never replaces the malformed existing file. Legacy optional
metadata defaults, empty rows after metadata, and a final row without a newline
remain supported.
Identifiers may contain spaces and UTF-8 text, but not slashes, backslashes,
control characters, leading dots, or leading/trailing spaces; they must fit in
the fixed 64-byte identifier field.

## Architecture

- `agent.c` owns live `Message` allocations and exposes a monotonic message
  revision.
- `session.c` owns the versioned disk format, workspace isolation, atomic I/O,
  identifier policy, active pointer, named-session creation, and listing.
- `session_manager.c` adapts live messages to persisted `SessionMessage` values,
  restores ownership into `agent.c`, and debounces autosaves.
- Autosave checkpoints dirty history at most once per 500 ms, including during
  streaming, while `/new` and session switching synchronously save dirty history
  first. Normal TUI shutdown also synchronously saves a dirty active session.
- `dispatch.c` owns `/new`, `/sessions`, and transient-state resets because those
  operations cross the C-owned UI and message state.

## Tests

`make test` covers workspace isolation, active pointers, sorting, title
creation, Unicode/multiline `text` and `raw_text` round trips, empty-placeholder
filtering, explicit named sessions, load-or-create selection, malformed
versions, complete JSON validation (including truncated objects and `1garbage`),
byte-for-byte preservation of malformed files on load-or-create, legacy
load/save round trips, oversized-row rejection, and `0600`/`0700` permissions. `make test-http-lua`
covers selected-session creation and restoration through the TUI
session manager. `make test-build` verifies create-then-resume headless
persistence in an isolated HOME as part of the linked-binary smoke checks.

Completion-review snapshots, held drafts, barriers and active scheduler handles
are transient run state, not resumable session metadata. Persisting final output
and issues does not restore an active review after restart. See
[completion review](completion-review.md).
