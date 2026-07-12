---
name: wiki-onboarding
description: Use when the user wants to set up Capstan Wiki or initialize its owner profile and starter folders. Guide them through preferences and a portable Markdown layout.
---

# Wiki Onboarding

Use this skill when the user asks to initialize Capstan Wiki, its owner profile,
or its starter layout.

Goal: guide the user to a useful first wiki with minimal friction.

## Flow

1. Explain briefly that Capstan Wiki is a normal Markdown directory, not hidden
   memory.
2. Ask for the minimum required preferences:
   - use Capstan's internal state directory by default:
     `$XDG_STATE_HOME/capstan/wiki`, falling back to
     `~/.local/state/capstan/wiki`;
   - what stable owner preferences should go into `profile/core.md`;
   - whether to create starter folders: `contexts/`, `resources/`, `pages/`,
     `sources/`.
3. If the user agrees, create the directory and starter files with available file
   tools. Do not overwrite existing files without asking.
4. Do not ask for a storage location or read permission for the default internal
   wiki. Discuss `wiki.path` only if the user explicitly requests a custom
   location.
5. After setup, suggest restarting Capstan so the wiki index is loaded into the
   system prompt.

## Normal missing-file cases

During first-time onboarding, a missing starter file or an empty default wiki is
expected. Do not inspect runtime logs for
these normal setup preconditions. Use logs only if a tool reports an unexpected
runtime/plugin/API failure that blocks onboarding.

## Starter layout

```text
wiki/
  WIKI.md
  profile/
    core.md
  contexts/
  resources/
  pages/
  sources/
```

`profile/core.md` should stay short and contain stable preferences only.
Larger notes should be Markdown files with frontmatter metadata, for example:

```markdown
---
schema_version: 1
id: example-context
kind: context
title: Example context
description: When this note is useful.
use_when:
  - The user asks about this topic.
tags:
  - example
index_policy: always
context_policy: retrieve_only
---

Full note body goes here. It is read only on demand with `wiki_read`.
```

Keep onboarding conversational and short. If the user has not provided enough
information, ask one focused question instead of doing many tool calls.
