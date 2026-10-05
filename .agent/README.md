# Wisp Local Agent System

This directory is a small navigation system for Open Code agents working with local Ollama models.

It is intentionally split into small linked files so an agent can move through the repository like a mind map instead of loading the whole project and all instructions into context.

## Start here

1. Read [Context Policy](CONTEXT_POLICY.md)
2. Read [Router](ROUTER.md)
3. Select a map from [Mind Maps](maps/index.md)
4. Select a skill from [Skills Index](skills/index.md)
5. Use [Task Template](TASK_TEMPLATE.md)
6. Finish with [Handoff Template](HANDOFF_TEMPLATE.md)

## Wisp focus

Wisp is a Windows-first terminal and shell project. Most work falls into one of these paths:

- build failure: [Build Map](maps/build.md)
- runtime bug or crash: [Debugging Map](maps/debugging.md)
- new shell/terminal/tool feature: [Architecture Map](maps/architecture.md) then [Implement Feature](skills/coding/implement-feature.md)
- safe cleanup: [Refactoring Map](maps/refactoring.md)
- tests or manual QA: [Testing Map](maps/testing.md)

## Before reading source

Use the editable memory files first:

- [Project Summary](memory/project-summary.md)
- [Commands](memory/commands.md)
- [Environment](memory/environment.md)
- [Conventions](memory/conventions.md)
- [Known Issues](memory/known-issues.md)
- [Known Decisions](memory/known-decisions.md)

## Context discipline

Do not scan all of `src/`, `tools/`, `tests/`, and `docs/` at once.

For Wisp tasks, identify the subsystem first:

- `src/core` for common memory/string/log/path helpers
- `src/shell` for lexer/parser/executor/builtins/history/jobs/completion
- `src/terminal` for screen buffer, VT parser, layout and renderer
- `src/platform` for config, keyboard input and ConPTY
- `src/main.cpp`, `src/window.cpp`, `src/repl.c` for app/UI wiring and REPL bridge
- `tools` for companion utilities
- `tests` for CTest entrypoints

## Handoff

Every agent should leave a compact result note using [Handoff Template](HANDOFF_TEMPLATE.md).

Record durable facts in:

- [Known Decisions](memory/known-decisions.md)
- [Known Issues](memory/known-issues.md)
- [Commands](memory/commands.md)
