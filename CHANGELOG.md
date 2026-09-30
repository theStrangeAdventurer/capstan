# Capstan v0.4.0

> **Date:** 2026-10-01
> **Range:** `v0.3.0` (`5c90e77`) → `HEAD` (`2595005`) — 27 commits.

## New Features

### Agent & Execution
- **Background subagents** — independent internal agents running in the background: queue, concurrency limits, retries only on transient errors, context and UI isolation, cancellation via `processes`.
- **Managed background processes** — long-running commands in the background with a control panel: status, output, and stop.
- **Hierarchical `max_tokens`** — resolution chain: model → provider → agent → default (32000), with a configuration example in `examples/config.lua`.

### Review & Quality
- **Background change review** — explicit request launches a background check; found issues are saved to the `issues` registry, verdicts and auto-fixes follow the protocol.
- **Issues registry** — persistent issues linked to a snapshot, with statuses and re-verification.

### Planning
- **Persistent task plans** — session artifact with a panel, statuses (`pending`/`in_progress`/`completed`/`blocked`/`cancelled`), revision, and cleanup.

### Observability
- **Native OpenTelemetry** — OTLP/HTTP traces and logs (opt-in), unified run measurements, session diagnostics, and structured lifecycle logs.

### Interface
- **Markdown rendering** — rendering with line wrapping, Unicode support, and text highlighting.
- **Animated start screen** — borderless, with a wave animation.
- **`terminal_guard`** — terminal recovery on fatal signals (SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE) via async-signal-safe `tcsetattr` + ANSI escape reset. Without it, a crash leaves the terminal unusable.
- **Session info panel** — available on demand.
- **VCS status in footer** — async display of working copy state.
- **Profile reasoning level** — choice persistence and hotkey configuration.

## Benchmark
- **DeepSeek v4 Pro (OpenRouter): Capstan 24/24 (100%), OpenCode 23/24 (95.8%)** — 2 runs, 12 tasks each.
- CPU usage 4.6× lower, RSS 20× smaller, wall time 23% faster.
- Updated demo screencast with four themes (latte, dracula, nord, tokyonight).

## Improvements
- Usage statistics persist across session resumes.
- Enforced configured limits; faster agent validation cycle.
- Refined TUI styling and task viewport / clipboard handling.
- Updated benchmark documentation and guides.

## Fixes
- Session header edge-case bug: pending space no longer eats the last byte before NUL.
- Correct input handling during wait and shell output processing.
- Collapsed shell output preserved across session resumes.
- External file permission enforcement; accurate tool activity status.
- Removed routing to a "weak" model: context compression and utility tasks now use the active model.
- Scope usage cache per owner: background runs no longer overwrite foreground session counters.

## Architecture / Refactoring
- Review baseline — repository `HEAD` instead of the snapshot at request time; new, deleted, and pre-existing edits are all accounted for.
- Review baseline routed through the VCS adapter; direct `git` hardcoding removed from the controller. Unsupported adapters fail with an explicit error (fail closed).
- Batch `HEAD` object reads (`git cat-file --batch`) instead of a process per file — eliminates UI blocking during baseline capture.

## Tests & Quality
- `make test` — unit tests (C), integration tests for terminal_guard, sessions, review.
- Lua tests: review controller, snapshots, issues, tasks, observability.
- Smoke build (`make test-build`) passed and CI binary publishing.