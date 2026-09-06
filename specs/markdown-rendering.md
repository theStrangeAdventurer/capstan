# Markdown rendering

## Behavior

The interactive message pane renders assistant Markdown with vendored MD4C
0.5.2 (MIT). `**strong**` / `__strong__`, `*emphasis*` / `_emphasis_` and nested
combinations become ncurses bold/italic attributes without delimiter characters.
Unclosed streaming delimiters remain literal until a subsequent update closes
them. Intraword underscores and escaped delimiters follow CommonMark rules.
Italic appearance depends on the terminal's terminfo/font support.

GFM pipe tables have Unicode borders, bold headers, column alignment from the
separator row, and wrapped cells. Widths use terminal cells, not UTF-8 bytes or
codepoint counts (including CJK, combining marks, and ordinary emoji). Columns
shrink to fit the message pane; when even minimum widths cannot fit, each body
row becomes labelled fields (`Header: value`), wrapped by the same line map.
A header-only narrow table displays its headers. Rows are separated for clarity.
GFM rules apply to missing/excess cells and escaped pipes.

Fenced code (backticks or tildes), inline code, tool-status blocks, tagged shell
output and unified diffs remain literal; existing diff colors and shell controls
are retained. An active Markdown fence closes even when the literal-line adapter
also marks its closing delimiter; fences originating entirely in tool output do
not open Markdown blocks in subsequent prose. Backtick fence info strings cannot
contain backticks: a line such as ```code``` is parsed as inline code, not an
opening block. Tilde fence info strings may contain backticks. Inline backticks
remain visible. User input/manual command output
is not parsed. Links retain their destinations; HTML and entities are displayed
as text, never executed. Other normal Markdown blocks use simple text headings,
lists and paragraph spacing. Ordered lists retain their starting number (including
zero) and increment per item; nested lists track type and counter independently.
This is a terminal renderer, not an HTML renderer.

## Architecture and ownership

- `markdown.c` owns the pure display projection: text, byte-indexed style flags,
  and original input offsets. MD4C owns Markdown syntax; no second inline parser.
- `tui.c` adapts tool-status/diff/shell provenance into literal source lines,
  projects only assistant messages after shell folding, and applies ncurses
  attributes. Generated borders/padding have no source offset, so they cannot
  activate shell controls. The projection is cached by message revision, pane
  width and shell expansion changes, not rebuilt on every idle animation tick.
- `text_layout.c` owns UTF-8 terminal-cell measurement and physical line wrapping.
  Table sizing, `linemap.c`, drawing, mouse hit-testing and selection use this
  shared policy. Visual navigation retains codepoint-based columns, converting
  to/from terminal cells at the UI boundary. Wrapping examines only the current
  row plus bounded UTF-8 lookahead, so wrapping long newline-free messages is
  linear in their byte length; trailing combining marks stay with their glyph.
- Scrolling derives its height from the same line map used by navigation. Copying
  uses the displayed projection (including table borders), not raw Markdown.
- Original `Message.text` / `raw_text`, model context, session persistence, logs,
  ACP, CLI run output and plugin/runtime contracts are unchanged. No ANSI codes
  are injected into model or persisted text.
- Allocation/parser failure falls back to unchanged raw text. A terminal narrower
  than a single wide glyph cannot display that glyph fully; layout still advances.
- MD4C is compiled into the single binary; no new dynamic/runtime dependencies.

## Tests and limitations

`make test` covers emphasis, nesting, escaped/unmatched delimiters, streaming
replacement, code/diff fences, exact table borders/alignment, cell styles,
Unicode widths, narrow/header-only/ragged tables, source offsets, wrapping and
visual mouse coordinates. `make test-tui-input` includes a real PTY Markdown
fixture, terminal attributes, resizing, unchanged model data, and existing shell
fold/click/input regressions. `make` validates ncurses/static-parser integration.

Terminal width uses libc `wcwidth` under the application's UTF-8 locale. Complex
ZWJ emoji sequences may differ from a terminal's grapheme rendering. Markdown
inside blockquote/list-contained fenced blocks follows MD4C's block handling;
plain top-level fences are preserved verbatim for existing diff highlighting.
