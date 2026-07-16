# Multimodal Images

Capstan can pass images returned by model tools to vision-capable
OpenAI-compatible models. The feature is tool-agnostic: Browser screenshots and
other MCP image tools use the same typed result path.

## Behavior

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

`agent/images.lua` owns image detection, base64 encoding, size limits, and MCP
image validation. MCP and local-file adapters return `{text, images}` only when
images exist. `agent/tools.lua` owns conversion from typed tool results to
conversation messages. Provider requests continue to serialize the canonical
message structure in `agent/runtime.lua`.

Image data must never be flattened into logs, status output, or token counting.
`agent/tokens.lua` uses a bounded image estimate instead of counting base64 as
text.

## Constraints

- A local path becomes image input only after `file_read` reads and validates
  the file signature; path text by itself is never sent as an image.
- Data URLs avoid requiring the remote provider to access the local filesystem.
- Automatic resizing or recompression is intentionally out of scope; rejecting
  an oversized image is the explicit failure mode.

## Tests

Provider-tool integration tests cover MCP and local-file image conversion,
message ordering, default image detail, base64 omission from logs, binary-file
safety, and preservation of the text-only path. Embedded-asset smoke testing
verifies the updated runtime is present in the standalone binary.
