# Popups

Popups support single-select and multi-select modes.

## Selection behavior

In single-select mode, focus and selection are the same state. Opening a popup
selects the first row. Moving the cursor with `j`/`k`, arrow keys, `g`/`G`, or
page movement clears the old selection and selects the focused row.
Single-select rows do not render checkbox markers because the focused row is the
selection.

In multi-select mode, focus and selection are independent. `l` selects the
focused row, `h` deselects it, and confirmation preserves the selected set. If
the user confirms a multi-select popup without selecting anything, the focused
row is selected as a fallback. Multi-select rows render `[ ]` and `[x]`
markers.

## Confirmation

`Enter`, `Tab`, and `l` on an already selected row confirm a popup. `Esc`
cancels and returns no selected values.

Permit-confirmation popups use the same movement keys for their horizontal
choice row: `h`/`k` and left/up arrows move backward, while `j`/`l` and
right/down arrows move forward. `Enter` and `Tab` confirm the highlighted
choice.

## Filterable lists

Typing `/` and pressing `Tab` or `Enter` opens the command list with a `Find:`
input. Printable keys fuzzy-filter and rank command names using the same finder
as model, file, and session popups. Backspace edits the query; arrows navigate
results; `Enter` or `Tab` inserts the selected command into the main input.

## Scrollbar

List popups render a slim scrollbar only when the item count exceeds the visible
row count. The scrollbar is drawn inside the popup's right edge and does not
change the popup width. Filterable popups start the scrollbar below the `Find:`
input row.

## Message popups

Message popups close with `Enter` or `Esc` and scroll with arrows, `j`/`k`,
`Ctrl-D`, and `Ctrl-U`. Error message popups can be copied to the system
clipboard by clicking them or pressing `c`/`y`; after copying, Capstan shows a
short non-modal acknowledgement.
