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
  scroll state, message cursor state, and token usage. Those values never cross
  session boundaries.
- Switching commits the durable `active` pointer before replacing the live
  session. If that atomic write fails, the loaded candidate is discarded and
  both the current messages and in-memory active session remain unchanged.
- Commands, including session switching, remain unavailable while a top-level
  agent run is active, following the queued-input command policy.
- After the first successful assistant response, Capstan asks the configured
  weak model for a concise 3–7 word title in the user's language. If no weak
  model is configured, the active model is used. Until that request succeeds,
  the UTF-8-safe beginning of the first user message is used as a local fallback.
  Generation runs as background HTTP work without tools or a loading spinner,
  does not enter conversation history or append to the visible assistant
  response, does not invoke `after_agent_turn` hooks, and is not canceled by
  stopping a user-visible response. It does not replace the title if the user
  has switched sessions before it completes. At most one title request may be
  in flight for a session.
- Empty assistant streaming placeholders are not persisted.
- `capstan run --session-id "my fucking bench"` creates a new session whose
  stable ID and title are exactly the supplied value. The title is marked
  explicit, so background title generation never replaces it. The prompt is
  persisted before provider work begins and non-empty assistant output is
  persisted when the run finishes.
- Named headless runs are create-only. An invalid or already existing ID fails
  closed before provider work, preventing accidental history replacement or
  ambiguous implicit resume behavior. Headless runs without `--session-id`
  remain one-shot and do not read or write sessions.

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

Session directories use Unix mode `0700`: only the owner can list, modify, or
enter them. Session and `active` files use `0600`: only the owner can read or
write them.

Invalid identifiers are rejected. Unsupported or malformed session files are
ignored by listing and fail closed when explicitly loaded; they do not crash TUI
startup. Reads cap individual JSONL rows at 4 MiB and sessions at 10,000
messages. A row exceeding the byte cap, an allocation failure, or an underlying
read error invalidates the whole load and is never treated as a clean EOF.
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
filtering, explicit named sessions, duplicate rejection, malformed versions,
oversized-row rejection, and `0600`/`0700` permissions. `make test-build`
provides the linked binary and embedded-runtime smoke checks.
