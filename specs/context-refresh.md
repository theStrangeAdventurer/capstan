# Context refresh (live skills and wiki)

Shared context roots stay live without restarting Capstan. Skills, the wiki
index and project/user instruction metadata are re-scanned from disk before
every top-level agent request, and an explicit command forces a rescan on
demand. Multiple Capstan instances on the same home therefore converge on the
same skills and wiki state through the filesystem itself.

## Behavior

- `agent/runtime.lua` rescans context metadata at the start of every top-level
  `runtime.run` — TUI dispatch, headless `capstan run`, ACP requests and
  background tasks — right before `_G.system_prompt` is captured for the run.
- Runs that pass an explicit `system_prompt` (subagents, completion review,
  dedicated model runs) never rescan. A run keeps one consistent prompt for
  all its tool continuations; freshness applies at run boundaries.
- The rescan re-renders the same prompt blocks as startup: skills index and
  summary, wiki index and summary, `capstan.skill_roots`, `capstan.wiki_path`
  and the user/project instruction sections. `/skills`, `/wiki status`,
  `agent.workspace` permission roots and the model-visible prompt all read
  these globals lazily, so every consumer sees the refreshed values.
- `/refresh` is a built-in no-history command: it rescans, shows the current
  skills and wiki summaries, and never triggers an agent request or model
  tokens. It is the orchestration entry point: an external orchestrator
  (Herdr, tmux, ACP host) can send `/refresh` to any pane to actualize shared
  context without a prompt.
- `capstan.refresh_context()` is the Lua surface of the same operation.
- Failure is fail-open: a failing rescan is logged (`context_refresh` runtime
  event) and the run proceeds with the previously rendered prompt. Partial
  scan failures follow startup semantics — unreadable `SKILL.md` files and
  malformed wiki index JSON are skipped, never fatal.
- Isolated runs (`--benchmark`) and `--no-wiki` keep their startup gating:
  refresh re-renders with the saved startup options instead of duplicating
  that policy.

## Architecture

- `src/plugins_runtime.c` owns one canonical re-render: the startup
  `load_system_prompt` path. `plugins_init_with_options` saves its
  `PluginsInitOptions`; `capstan.refresh_context` simply re-runs
  `load_system_prompt` with them. There is no second scanner, renderer or
  policy copy.
- `_G.system_prompt` remains the single source of truth; each request reads
  it at build time (`build_messages`), so replacing the global is the whole
  update mechanism. No prompt caching exists beyond a run.
- Scans are read-only and lock-free. Concurrent instances never coordinate:
  each re-reads the shared roots and degrades to startup semantics on
  partially written files. A wiki index JSON being replaced by another
  instance mid-scan is non-fatal by the existing wiki index contract.
- The wiki scan is stat-gated: `wiki_build_prompt_cached` /
  `wiki_build_summary_cached` probe the tree with `stat` only — path, mtime
  and size per file, mirroring the render walk — and return cached renders
  byte-identical while the file set is unchanged. Full reads, parsing and
  rendering run only after a real add, remove or change. Second-granularity
  same-size edits can be missed until any other input changes; permission-only
  changes are not tracked; a failed probe forces a rebuild rather than
  trusting a partial file set. Skills and instruction files stay ungated by
  design: they are owner-curated, small and bounded, while the wiki is the
  only root that may grow without bound.
- True filesystem event watching (FSEvents/inotify/kqueue) was deliberately
  not used: events can be dropped, still require a full-scan fallback and a
  second per-platform implementation, while the stat probe costs microseconds
  on owner-scale wikis and needs no event-loop integration.

## Future: Herdr orchestration

Herdr reports already attach resume argv and states; actualizing shared
context needs no new protocol:

- Any prompt sent to a pane refreshes automatically at the next request.
- For immediate, zero-token actualization, an orchestrator sends `/refresh`
  to selected panes; the visible summaries double as a confirmation.
- No cross-instance locking, sockets or broadcast are required for the
  filesystem-shared parts; only features that need coordinated writes
  (for example instructing one instance to edit wiki another instance
  consumes) would need an explicit orchestration layer on top.

## Test notes

- `test/test_context_refresh.lua` (offline, part of `make test-http-lua`)
  covers: one rescan per top-level run before provider resolution, the
  explicit-prompt skip, fail-open logging of a failing rescan, and the
  `/refresh` command handler (success, summaries, failure, unavailable
  runtime, no-history flag).
- `test/test_wiki.c` covers the stat gate: byte-identical cached renders on
  an unchanged tree, invalidation on edits with new sizes, added/removed
  documents, index JSON changes, and NULL-cache degradation to the uncached
  builders.
- `make test-build` embeds `plugins/refresh.lua` and asserts `/refresh`
  exists in the self-test command list.
- Existing suites (`test_skills_plugin`, `test_wiki_plugin`, SIWC runtime
  fixtures) run with stubbed `capstan` tables and no `refresh_context`, so
  the guarded trigger is skipped and their behavior is unchanged.
