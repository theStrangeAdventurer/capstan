# OpenRouter performance and upstream evidence

## Observed problem

The local 20261009 comparison used DeepSeek V4 Pro, medium reasoning, and a
240-second task timeout. Its aggregate wall-time ratio was 1.79. Retrospective
analysis found all 42 completed Capstan responses longer than 500 tokens below
60 tokens/sec after first semantic output; only 10 of 41 OpenCode responses
were below that threshold. Both clients sometimes had the same ~40 tokens/sec
regime. This local evidence does **not** identify an upstream: historic Capstan
traces dropped response identities and cache counters. Partial timeout traces
also omit unfinished-request usage, so their token totals are lower bounds.
OpenCode `step_start` is not request start; use its `message=stream` log and
message/tool timestamps to avoid excluding initial wait from the comparison.

## Request policy

`agent/provider_config.lua` owns provider routing configuration and applies it
through the common request builder before request hooks. Built-in OpenRouter
now defaults to `routing = {sort = "throughput"}`. This follows OpenRouter's
[documented provider sorting](https://openrouter.ai/docs/guides/routing/provider-selection)
instead of its default price-based load balancing. This is an explicit speed
preference, not evidence about which backend served the old run or a measured
speedup. It may increase cost or change model-serving characteristics.

A user table fully replaces the default. `routing = false` or `{}` omits the
wire `provider` object. Pin with `routing = {only = {"provider-slug"},
allow_fallbacks = false}`. Preferences are deep-copied per request, including
nested lists and price caps. No changes to the model, reasoning effort, output
budget, public task prompt, or timeout are used as speed shortcuts. Other
providers have no default routing object. TUI, headless CLI, subagents and
retries share this policy; persistence is existing user config only. Request
hooks may still adapt the body. API errors are surfaced without silently
removing restrictions or retrying auth/billing errors.

## Lossless streaming and evidence

The OpenAI-compatible SSE parser previously returned early for `usage`, then
reasoning, then tools. A packet carrying multiple fields could lose text or a
tool call. It now expands all fields into individual typed chunks, reasoning
before text/tools, usage last. A packet containing both reasoning aliases uses
one textual alias; structured reasoning details are retained independently.
Single-chunk parser overrides and stream hooks retain their existing contract.
Missing data is not fabricated; malformed/non-object events are ignored.

Known SSE identity fields are bounded/redacted and retained even on metadata-only
packets: `id` -> `response_id`, `model` -> `response_model`, `provider` ->
`upstream_provider`. They propagate to runtime logs, native model spans, and
headless model-completion traces. Legacy traces additionally retain normalized
`usage.cached_tokens`, including explicit zero; absent values stay null. No
headers or prompt contents are added to logs. Missing backend identity cannot
be reconstructed from requested model/provider configuration.

This parser bug is independently reproducible; it has not been established as
the source of the historic 1.79 ratio.

## Controlled network diagnostic

`benchmarks/polyglot/scripts/probe_openrouter.py` sends four identical synthetic
coding requests using the same Python transport. It varies only auto/throughput
routing and absent/OpenCode attribution headers. It is a microdiagnostic, not
an agent comparison, and does not pretend to capture either agent's wire body.
It reads SSE without waiting for a large buffer to fill, records upstream,
generation ID, usage and timing, and aborts on the first API/transport error.
There is no local proxy, no credential logging, and no automatic retry.

```sh
python3 benchmarks/polyglot/scripts/probe_openrouter.py \
  --output benchmarks/work/openrouter-probe-dry.jsonl --dry-run
python3 benchmarks/polyglot/scripts/probe_openrouter.py \
  --output benchmarks/work/openrouter-probe-live.jsonl --repeats 2
```

The live command makes eight paid requests. Repeat with `--only PROVIDER_SLUG`
to pin an observed upstream with fallbacks disabled. Alternating repetition
order reduces time-order effects. Different reported upstreams plus different
speeds are evidence of route selection; attribution causality requires the
controlled comparison, not an assumption. When pinning removes the speed gap,
that supports the routing hypothesis. If identities are absent, the saved
`response_id` can be used with OpenRouter's generation endpoint.

After diagnosing the route, rerun the canonical unchanged 12-task benchmark
for both agents with the **same explicit upstream restrictions** and then the
intended production routing settings, using fresh comparison IDs. Do not fold
these changed-policy runs into the old comparison. Report score and wall time,
including timeouts. Dirty worktrees remain exploratory.

The OpenCode harness accepts `--provider-sort throughput` for the same speed
preference as Capstan, and `--upstream PROVIDER_SLUG` to disable fallback and
pin a backend. These merge into the model's `options.provider`, alongside the
existing medium reasoning variant. They do not modify public task prompts.
For the old automatic-routing comparison, set Capstan `routing = false` and
omit both OpenCode flags. Record these choices in the comparison configuration.

## Validation and limits

`test/test_stream_packets.lua` exercises combined packets, one-byte fragmentation,
CRLF and trailing packets, metadata-only events, provider errors, hook suppression,
custom parsers, missing usage, and routing defaults/replacement/deep-copy behavior.
Runtime timeline tests check routing reaches actual serialized child requests.
`test/test_openrouter_probe.py` checks timing normalization and request equivalence.
Existing provider and telemetry suites cover compatibility; compile and embedded
build smoke checks cover the native adapters.

The development sandbox for this change denies DNS/network access and local
socket bind. Live upstream attribution, measured acceleration and parity with
OpenCode therefore remain unverified. Offline tests do not establish benchmark
quality or speed.
