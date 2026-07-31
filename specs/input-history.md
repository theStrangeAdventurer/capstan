# Input History

Capstan stores a small prompt-entry history for the interactive TUI input box.
This history is separate from the model conversation history and is used only to
recall previously submitted prompts.

## Behavior

- Up arrow in input focus recalls older submitted prompts.
- Down arrow moves toward newer prompts and restores the draft that was present
  before history browsing started.
- Empty submissions are not saved.
- Consecutive duplicate submissions are stored once.
- History is workspace-scoped and persists across Capstan restarts.
- Headless `capstan run` does not read or write interactive input history.

## Storage

History is stored under the Capstan state directory:

```text
~/.local/state/capstan/history/<workspace-hash>.jsonl
```

If `XDG_STATE_HOME` is set, Capstan uses:

```text
$XDG_STATE_HOME/capstan/history/<workspace-hash>.jsonl
```

Each line is one JSON string so multiline prompts can be stored safely. Capstan
keeps the latest 20 prompts per workspace.

## Implementation Notes

`input_history.c` owns persistence and navigation state. `dispatch_submit()`
records successful non-empty input before clearing the input buffer. This
includes text accepted into the active run's queued-input FIFO, but excludes a
sixth message rejected because the queue is full. `main.c` loads history during
TUI startup and routes `KEY_UP` / `KEY_DOWN` to history navigation while input
focus is active.

## Tests

Unit tests cover workspace isolation, JSONL persistence, consecutive duplicate
suppression, the 20-entry limit, and draft restoration after browsing.
