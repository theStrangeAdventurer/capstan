# Capstan Mind With External Raw Sources

## Summary

Capstan Mind will use `~/.config/capstan/mind.git` as the canonical bare repo for agent-maintained knowledge, but it will not copy the user's existing knowledge base into it. Existing KB repos are registered as read-only external sources through source manifests. Capstan reads and indexes those sources, then writes only derived notes, links, profile data, decisions, and synthesis into Mind.

## Key Changes

- Mind storage:
  - `~/.config/capstan/mind.git` is the bare canonical repo.
  - `~/.config/capstan/mind/` is a checkout/worktree for inspection and Obsidian.
  - External KB repos stay in their own locations and are referenced from `mind/sources/*.md`.
- Source manifests:
  - Store `id`, `type=git-repository`, local `path`, optional `remote`, `branch`, `content_roots`, `exclude`, and `mode=read_only`.
  - Capstan records source provenance in generated notes using source id, relative path, and current commit hash.
- Raw/source separation:
  - External source repos are read-only by default.
  - `mind.git` stores derived wiki/profile/project/decision/insight notes, not raw copies.
  - Later patch-back support can be added as an explicit mode, but v1 will not write to source repos.
- Search and indexing:
  - Metadata/FTS index covers both Mind notes and registered external sources.
  - Future vector index stores chunks derived from both, keyed by `source_id`, `path`, `commit`, and `content_hash`; vectors are cache only, not source of truth.
- API and slash commands:
  - `/mind source add <path>` registers an external KB repo.
  - `/mind source list`, `/mind source status`, `/mind source scan <id>`.
  - `/mind search <query>` searches Mind notes plus registered sources.
  - `/mind read <id-or-source-path>` reads a Mind note or external source file.
  - `/mind ingest <source-id>` creates/updates derived wiki notes from selected source material.
  - Lua/tools: `mind_source_add`, `mind_source_list`, `mind_search`, `mind_read`, `mind_ingest`, `mind_note_write`.

## File Model

- Mind checkout structure:

  ```text
  mind/
    AGENTS.md
    index.md
    log.md
    sources/
      personal-kb.md
    wiki/
    profile/
    projects/
    decisions/
    insights/
  ```

- Example source manifest:

  ```md
  ---
  id: source-personal-kb
  type: git-repository
  name: Personal Knowledge Base
  path: /Users/alxd/kb
  remote: git@github.com:alxd/kb.git
  branch: main
  mode: read_only
  content_roots: ["."]
  exclude: [".git", "node_modules", ".obsidian"]
  ---

  Existing personal knowledge base. Treat as raw source of truth.
  ```

- Generated notes cite sources:

  ```yaml
  sources:
    - source: source-personal-kb
      path: projects/capstan/permissions.md
      commit: abc123
  ```

## Test Plan

- Unit tests for source manifest parsing and validation.
- Unit tests that source indexing records path, source id, commit, title/summary metadata, and does not copy source files into `mind.git`.
- Provider/plugin tests for `/mind source add`, `/mind search`, and `/mind read`.
- Prompt tests proving only metadata/index snippets enter context, not full external source bodies.
- Git tests using temp bare repo plus temp external KB repo.

## Assumptions

- Existing KB repos are read-only in v1.
- Mind's generated wiki is allowed to summarize and link to external source files, but raw source content remains outside `mind.git`.
- Manifest pointer is the chosen source link mode; no submodules or vendored copies in v1.
- Vector search is planned as a derived cache layer after the Git/manifest/index workflow is stable.
