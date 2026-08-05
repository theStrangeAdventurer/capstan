# Multimodal Images

Capstan can pass images returned by model tools to vision-capable
OpenAI-compatible models. The feature is tool-agnostic: Browser screenshots and
other MCP image tools use the same typed result path.

## Behavior

- In the TUI, `Ctrl+V` reads an image from the system clipboard and attaches it
  to the current prompt as `[Image N]`. Multiple images may be attached; with
  an empty text field, Backspace removes the most recent attachment.
- Clipboard images are normalized to PNG, limited to 10 MiB, and sent as
  structured `image_url` content rather than marker text. Image-only prompts
  are valid and attachments persist with the project session.
- MCP `content` items with `type = "image"`, a valid `image/*` MIME type, and
  base64 `data` are retained.
- Local PNG, JPEG, GIF, and WebP files read through `file_read` use the same
  typed image path. Format detection uses file signatures rather than trusting
  filename extensions.
- Each decoded image is limited to 10 MiB. Invalid or oversized items become a
  short text notice; they are not sent as image input.
- The required `role = "tool"` message remains textual. After all tool results,
  Capstan appends one `role = "user"` message containing a short text block and
  OpenAI-compatible `image_url` data-URL blocks. This preserves tool-call
  ordering while making the pixels available to the next model turn.
- Text-only tools keep their existing string result contract.
- The provider must support vision input. A provider rejection is surfaced as
  its normal API error.

## Architecture

`agent/images.lua` owns tool-image detection, base64 encoding, size limits, and
MCP image validation. `src/clipboard.c` owns platform clipboard acquisition;
`src/input.c` owns pending TUI attachments; and `Message` plus session storage
own submitted clipboard images. MCP and local-file adapters return
`{text, images}` only when images exist. `agent/tools.lua` owns conversion from
typed tool results to conversation messages. Provider requests continue to
serialize the canonical message structure in `agent/runtime.lua`.

Image data must never be flattened into logs, status output, or token counting.
`agent/tokens.lua` uses a bounded image estimate instead of counting base64 as
text.

## Constraints

- Clipboard acquisition uses native platform commands without a shell: AppleScript
  on macOS, `wl-paste` on Wayland, and `xclip` on X11. Linux users need the
  corresponding clipboard utility installed.
- Clipboard attachment is available only in the interactive TUI. ACP clients
  read their own clipboard and send standard ACP image content blocks.
- Image prompts are not accepted into the bounded input queue while another run
  is active; Capstan preserves the draft and reports the limitation.
- A local path becomes image input only after `file_read` reads and validates
  the file signature; path text by itself is never sent as an image.
- Data URLs avoid requiring the remote provider to access the local filesystem.
- Automatic resizing or recompression is intentionally out of scope; rejecting
  an oversized image is the explicit failure mode.

## Tests

Unit tests cover clipboard base64 encoding, pending attachment behavior, and
session image persistence. Provider-tool integration tests cover MCP and
local-file image conversion, message ordering, default image detail, base64
omission from logs, binary-file safety, and preservation of the text-only path.
Embedded-asset smoke testing verifies the updated runtime is present in the
standalone binary.

The local-file integration test uses `test/fixtures/vision-shapes.png`, a real
PNG containing a blue square on the left and a red triangle on the right. It
asserts that the canonical Chat Completions request keeps the textual tool
result, then appends a user content array whose text block precedes a PNG
`image_url` data URL with `detail = "auto"`.

An opt-in live smoke test verifies the same fixture against OpenRouter's
`/api/v1/chat/completions` endpoint and requires the selected vision model to
identify both shapes:

```sh
OPENROUTER_API_KEY=... make test-openrouter-vision
```

The default live model is `minimax/minimax-m3`. Set
`OPENROUTER_VISION_MODEL` to test another OpenRouter model with image input.
The live target is deliberately excluded from `make test` because it requires
network access, credentials, and incurs provider usage.
