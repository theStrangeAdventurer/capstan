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

UTF-8 input (including Cyrillic) is assembled from terminal bytes before
filtering; incomplete/invalid sequences are never published as query text.
Backspace removes a complete code point. The 255-byte query limit never splits
a character. Query display follows its tail, with character-based clipping and
cursor placement; list labels and titles also avoid splitting UTF-8 characters.
The existing finder owns ranking (no additional Unicode case-folding policy).
Pure popup regressions cover split input, deletion, malformed sequences, buffer
limits, and empty-result navigation. PTY checks should type Cyrillic into a
filterable list and verify its visible query, cursor and selected result.

## Scrollbar

List popups render a slim scrollbar only when the item count exceeds the visible
row count. The scrollbar is drawn inside the popup's right edge and does not
change the popup width. Filterable popups start the scrollbar below the `Find:`
input row.

## Choice modal

`popup.choice(title, message, choices)` renders a blocking single-choice modal.
The title is drawn in the border, the message is shown as a single clipped line,
and each choice is a `[N]` row. `j`/`k` and arrow keys move the highlight,
`1`–`9` select a row directly, `Enter` or `Tab` confirms, and `Esc` cancels
returning `nil`. It is the synchronous modal used for review outcomes that need
an explicit user decision (for example fix now versus ask later).

## Message popups

Message popups close with `Enter` or `Esc` and scroll with arrows, `j`/`k`,
`Ctrl-D`, `Ctrl-U`, or the mouse wheel. Dragging over message text highlights
and copies the selected source text on release, in either direction. Soft wraps
never insert newlines into copied text; real newlines are preserved. Selection
and wrapping use the same UTF-8/cell-aware layout as rendering, including after
scrolling or resizing. Starting selection disables an error popup's expiry.
`c`/`y` copies an informational popup's selection (or its full body if none).
Error message popups retain whole-message `c`/`y` copying and a short non-modal
acknowledgement; a plain click on non-link error text also copies the error.

A plain click on any visible fragment of a whitespace-delimited HTTP(S) URL
opens the complete source URL using the desktop browser opener, including all
query parameters, even across multiple wrapped rows. Dragging a URL selects
text instead of opening it. No arbitrary URI schemes or shell interpolation
are used. This applies during blocking `/connect` authorization as well as the
normal input loop; authorization URLs remain UI-only, not chat/history/log data.

Regression tests cover source offsets and full URL resolution across widths,
UTF-8 boundaries, real versus soft newlines, and non-HTTP/control rejection.
PTY mouse tests cover rendered wrapped URLs and clipboard/browser arguments
using isolated fake openers and clipboard commands, never real credentials.
