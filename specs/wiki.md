# Capstan Wiki

Capstan Wiki is a portable, git-backed owner context repository. It is not hidden model memory: it is a normal Markdown directory that the owner can edit, version, sync, and inspect.

## Goals

- Keep a small owner profile always available to Capstan.
- Expose a compact metadata index of larger wiki documents, similar to the skill index.
- Load full wiki documents only on demand.
- Keep private Capstan-only context out of project `AGENTS.md`.
- Avoid vector search in the first version.

## Repository layout

The default wiki root is `$XDG_STATE_HOME/capstan/wiki`, falling back to
`~/.local/state/capstan/wiki`. Capstan creates this internal directory at
startup. It can be overridden with `wiki.path` in
`~/.config/capstan/config.lua`:

```lua
return {
  wiki = {
    path = "~/Documents/capstan-wiki",
  },
}
```

Recommended layout:

```text
wiki/
  WIKI.md
  profile/
    core.md
  contexts/
    homelab.md
  resources/
    devices.md
  pages/
    decisions/
    concepts/
  sources/
```

## Context model

The wiki may grow without bound. The model context must stay bounded.

Capstan loads:

1. `profile/core.md` fully, if present. This file must stay small and contains stable owner rules and preferences.
2. A metadata-only wiki index for discoverable Markdown documents in the wiki.
3. A metadata-only ingest index for external Markdown roots under `wiki/index/*.json`.

Capstan does not load full wiki documents from the index automatically in C. The agent must use the `wiki_read` model tool or `/wiki read` command when a task matches the metadata.

Capstan also does not load full external source documents automatically. The agent must use `wiki_source_read` with the source id and path shown in the wiki index.

## Discoverable documents

Wiki Markdown files can include frontmatter:

```yaml
---
schema_version: 1
id: home-devices
kind: resource
title: Home devices
description: Домашние устройства владельца, роли машин и SSH aliases.
use_when:
  - Пользователь спрашивает про NAS, медиасервер, Jellyfin или домашний SSH.
tags:
  - homelab
  - ssh
index_policy: always
context_policy: retrieve_only
---
```

Only frontmatter metadata is included in the wiki index. The body is not included until the document is read explicitly.

`profile/core.md` is special: it is included fully and does not need to appear in the metadata index.

## Initial commands and tools

- `/wiki` or `/wiki status` shows configured wiki path and loaded index summary.
  If `wiki.path` is missing, it returns a concise onboarding handoff instead of
  a dead-end error: the agent should use the built-in `wiki-onboarding` skill,
  ask where to store the wiki, collect stable owner preferences, and offer to
  create the starter layout and config.
  Missing wiki directories or missing wiki config during this first-time flow
  are normal setup preconditions, not runtime failures; the agent should not
  inspect logs for them unless a tool reports an unexpected blocking error.
- `/wiki read <relative-path>` reads a wiki file.
- `/wiki ingest <path>` indexes an external Markdown file or directory without
  copying source files into the wiki. The command sends bounded Markdown
  snippets to a one-turn weak-model run, asks for strict JSON metadata, and
  stores the resulting source index at `wiki/index/<source-id>.json`. If the
  weak-model run times out, errors, or returns malformed metadata, Capstan still
  writes a fallback external index from local file paths and hashes.
- `/wiki ingest --copy <path> [target-dir]` also copies Markdown into the wiki
  under `target-dir` (default `sources/<source-id>`) and writes generated
  frontmatter only to the copied wiki files. Source files are never modified.
- `wiki_read` model tool reads a wiki file by relative path.
- `wiki_ingest` model tool indexes an external Markdown file or directory when
  the user asks the agent to add, index, summarize, or make that source
  available to the wiki. It uses the same ingest pipeline as `/wiki ingest`.
  Parameters are `path`, optional `copy`, and optional `target_dir` for copy
  mode.
- `wiki_source_read` model tool reads a Markdown file from an external source
  root that was previously recorded by `wiki_ingest` or `/wiki ingest`.

Expected `wiki_read` and `wiki_source_read` failures, including missing files,
invalid paths, and absent source entries, use the plugin error result contract.
They appear as failed tool rows with a concrete explanation and do not require
runtime-log inspection.

All wiki reads are constrained to the effective wiki root and reject absolute
paths or parent traversal. `wiki_read` does not request permission because the
wiki is Capstan-owned internal state. `wiki_ingest` still uses the normal
`file_read` permission prompt for its external source path, so the user
explicitly approves reading that file or directory. Any positive `wiki_ingest`
prompt choice persists that permission. `wiki_source_read` bypasses a second
prompt because ingest is explicit consent for that source root, but validates
every read against `wiki/index/<source-id>.json` and only reads indexed paths.

If a model sends the generic `file_read` tool an absolute or workdir-relative
path inside the configured wiki, Capstan routes it to `wiki_read` before
permission handling. This preserves the internal-Wiki policy and never grants
the generic file reader access outside the wiki root.

## Ingest index

Each ingest source root owns one JSON file:

```text
wiki/
  index/
    docs-1a2b3c4d.json
```

The index schema is intentionally simple and generated by Capstan:

```json
{
  "schema_version": 1,
  "source_id": "docs-1a2b3c4d",
  "source_root": "/absolute/source/root",
  "indexed_at": "2026-07-09T00:00:00Z",
  "generated_by": "wiki-ingest",
  "mode": "external",
  "entries": [
    {
      "path": "guide.md",
      "id": "guide",
      "kind": "source",
      "title": "Guide",
      "description": "One sentence.",
      "use_when": ["When this file helps."],
      "tags": ["docs"],
      "index_policy": "always",
      "context_policy": "retrieve_only"
    }
  ]
}
```

Repeated ingest of the same source root replaces that root's JSON file. The C
prompt builder treats malformed index JSON as non-fatal and ignores it.

## Weak model usage

Wiki ingest uses `capstan.models.weak()` when a weak model is configured and
falls back to the active provider/model otherwise. The weak-model run is
non-interactive and bounded:

- `tools = {}`
- `max_turns = 1`
- `update_status = false`
- `update_usage = false`

The weak model receives truncated Markdown snippets and must return strict JSON
metadata only. The main agent then sees only the generated metadata until it
chooses `wiki_read` or `wiki_source_read`.

Weak-model metadata generation is best-effort. Provider errors, timeouts, and
malformed JSON must not make index-only ingest fail after the source has been
scanned successfully. In those cases Capstan writes fallback entries with the
source-relative path as the title, empty descriptions/tags/use_when, and
`context_policy = "retrieve_only"`. The fallback index still uses
`mode = "external"` unless the user explicitly requested copy mode.

## Agent synthesis flow

When the user asks the agent to summarize, intersect, or materialize external
Markdown sources into wiki notes, the agent should:

1. Call `wiki_ingest` for the user-specified source path if it is not already indexed.
2. Use the resulting index metadata to choose relevant `source/path` entries.
3. Read selected source files with `wiki_source_read`.
4. Write derived Markdown inside `wiki.path` with normal file tools, including
   frontmatter and source provenance where useful.

## Relationship to skills

Skills remain procedures. Wiki documents are knowledge and context.

The wiki reuses the skill discovery idea: the agent sees compact metadata and decides when to load the full document. Unlike skills, wiki metadata is advisory; matching a wiki document does not have priority-zero mandatory semantics.
