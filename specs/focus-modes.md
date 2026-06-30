# Focus Modes

The app has two focus modes: **INPUT** (default) and **MESSAGES**.
Ctrl-F switches between them. Shift-Tab cycles the active agent profile. The
status bar always shows the current mode.

## Focus states

| Mode | Description | Status bar | Visual cues |
|------|-------------|-----------|-------------|
| `FOCUS_INPUT` | Typing in the input box | `-- INSERT -- Ctrl-F:focus` | Bold input box border, visible cursor in input |
| `FOCUS_MESSAGES` | Navigating message history | `-- MESSAGES -- v:select Esc:focus` | Dim input box + content, block cursor in messages |
| `FOCUS_MESSAGES` + visual | Selecting text in messages | `-- VISUAL -- y:yank Esc:cancel` | Dim input box, `A_REVERSE` selection highlight in messages |

## Key bindings

**Global (both modes):**

| Key | Action |
|-----|--------|
| `Ctrl-F` | Toggle focus: INPUT ↔ MESSAGES |
| `Shift+Tab` | Cycle active profile: fast -> implement -> plan |
| Scroll wheel | Scroll message history |
| Click in messages | Focus MESSAGES and place the message cursor |
| Drag in messages | Select message text from press point to release point |
| `PgUp`/`PgDn` | Page scroll (5 lines) |

Manual scroll disables tail-follow while output is streaming. When the user
scrolls up, newly appended agent text must not move the visible message window;
the viewport stays anchored to the same message lines. Scrolling back to the
bottom (`scroll == 0`) re-enables tail-follow, and new submissions reset scroll
state to the bottom.

In input mode, `Tab` opens command and autocomplete popups when the current
input starts with a command. When a popup is active, popup key handling takes
precedence over focus mode switching. In selection and
[permit-confirmation](permissions.md) popups, `Tab` confirms the current choice,
matching `Enter`.

**FOCUS_INPUT:**

| Key | Action |
|-----|--------|
| Printable chars | Insert into input buffer |
| `Enter` | Submit input |
| `←`/`→` | Move cursor in input |
| `Backspace` | Delete character |

**FOCUS_MESSAGES (navigation):**

| Key | Action |
|-----|--------|
| `j`/`↓` | Move cursor down |
| `k`/`↑` | Move cursor up |
| `Ctrl-D` | Scroll down by half a page |
| `Ctrl-U` | Scroll up by half a page |
| `h`/`←` | Move cursor left |
| `l`/`→` | Move cursor right |
| `w` | Jump to next word start |
| `b` | Jump to previous word start |
| `0` | Jump to line start |
| `$` | Jump to line end |
| `v` | Start visual selection (anchor at cursor) |
| `Esc` | Return to INPUT |

**FOCUS_MESSAGES (visual selection):**

| Key | Action |
|-----|--------|
| `j`/`k`/`h`/`l`/`w`/`b`/`0`/`$` | Move cursor, extend selection |
| `y` | Yank selection to clipboard, exit visual |
| `Esc` | Cancel selection, stay in MESSAGES |

## Transitions

```
INPUT ──Ctrl-F──▸ MESSAGES (cursor at last line)
MESSAGES ──Ctrl-F──▸ INPUT (cursor in input box)
MESSAGES ──Esc──▸ INPUT (only without active selection)
MESSAGES ──v──▸ VISUAL (selection starts)
VISUAL ──y──▸ MESSAGES (yank, selection cleared)
VISUAL ──Esc──▸ MESSAGES (selection cancelled)
INPUT ──click messages──▸ MESSAGES (cursor at clicked text)
MESSAGES ──drag messages──▸ VISUAL (selection remains after release)
```

## Clipboard

- macOS: `pbcopy`
- Linux: `xclip -selection clipboard`, fallback `xsel --clipboard`

Mouse selection uses the same visual selection state as keyboard selection.
Releasing the mouse ends drag tracking and yanks the selected text automatically.
Keyboard visual selection uses `y` to yank. Both copy paths briefly flash the
selected range, use the normal clipboard path, and show a non-modal
`Text copied` toast for 500ms.

## Architecture

- **`mode.c`** — `mode_get()`, `mode_set()`, `mode_toggle()` — focus state
- **`visual.c`** — cursor position, selection range, yank, word navigation
- **`linemap.c`** — maps each visible line to its source message + byte range
- **`tui.c`** — renders mode-dependent visual cues (bold/dim border, status bar, block cursor, selection highlight)
- **`main.c`** — routes keys based on `mode_get()` and `visual_is_active()`
- **`set_escdelay(50)`** — called before `initscr()` so Esc responds instantly
