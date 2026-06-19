# Popups

Popups support single-select and multi-select modes.

## Selection behavior

In single-select mode, focus and selection are the same state. Opening a popup
selects the first row. Moving the cursor with `j`/`k`, arrow keys, `g`/`G`, or
page movement clears the old selection and selects the focused row.

In multi-select mode, focus and selection are independent. `l` selects the
focused row, `h` deselects it, and confirmation preserves the selected set. If
the user confirms a multi-select popup without selecting anything, the focused
row is selected as a fallback.

## Confirmation

`Enter`, `Tab`, and `l` on an already selected row confirm a popup. `Esc`
cancels and returns no selected values.
