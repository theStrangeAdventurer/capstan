# Focus Modes

The app has two focus modes: **INPUT** (default) and **MESSAGES**.
Shift+Tab switches between them. The status bar always shows the current mode.

## Focus states

| Mode | Description | Status bar | Visual cues |
|------|-------------|-----------|-------------|
| `FOCUS_INPUT` | Typing in the input box | `-- INSERT -- Shift+Tab:focus` | Bold input box border, visible cursor in input |
| `FOCUS_MESSAGES` | Navigating message history | `-- MESSAGES -- v:select Esc:focus` | Dim input box + content, block cursor in messages |
| `FOCUS_MESSAGES` + visual | Selecting text in messages | `-- VISUAL -- y:yank Esc:cancel` | Dim input box, `A_REVERSE` selection highlight in messages |

## Key bindings

**Global (both modes):**

| Key | Action |
|-----|--------|
| `Shift+Tab` | Toggle focus: INPUT ↔ MESSAGES |
| Scroll wheel | Scroll message history |
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
INPUT ──Shift+Tab──▸ MESSAGES (cursor at last line)
MESSAGES ──Shift+Tab──▸ INPUT (cursor in input box)
MESSAGES ──Esc──▸ INPUT (only without active selection)
MESSAGES ──v──▸ VISUAL (selection starts)
VISUAL ──y──▸ MESSAGES (yank, selection cleared)
VISUAL ──Esc──▸ MESSAGES (selection cancelled)
```

## Clipboard

- macOS: `pbcopy`
- Linux: `xclip -selection clipboard`, fallback `xsel --clipboard`

## Architecture

- **`mode.c`** — `mode_get()`, `mode_set()`, `mode_toggle()` — focus state
- **`visual.c`** — cursor position, selection range, yank, word navigation
- **`linemap.c`** — maps each visible line to its source message + byte range
- **`tui.c`** — renders mode-dependent visual cues (bold/dim border, status bar, block cursor, selection highlight)
- **`main.c`** — routes keys based on `mode_get()` and `visual_is_active()`
- **`set_escdelay(50)`** — called before `initscr()` so Esc responds instantly
