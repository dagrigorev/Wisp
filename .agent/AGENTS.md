# Agents

Use this file to orient local agents before selecting a role.

## Local model constraints

The project is intended to work with local Ollama models through Open Code.

Assume limited context size.

Therefore:

- Prefer navigation files over large global context.
- Read only the files needed for the current skill.
- Summarize findings before moving deeper.
- Avoid loading generated files, build directories, dependency folders, binaries, logs, screenshots, and large artifacts unless directly needed.
- Use `.agent/memory/commands.md` instead of rediscovering commands every time.
- Use `.agent/memory/project-summary.md` before scanning the whole repository.
- Prefer `docs/ARCHITECTURE.md` over recursively reading all source files when choosing a subsystem.

## Wisp agent routing

- Build or linker failure: [Release Engineer Agent](skills/agents/release-engineer.md) or [Debugger Agent](skills/agents/debugger.md).
- Runtime crash, wrong terminal behavior, pane/input/scrollback bug: [Debugger Agent](skills/agents/debugger.md).
- New feature in shell, terminal, UI, config, or tools: [Architect Agent](skills/agents/architect.md) then [Implementer Agent](skills/agents/implementer.md).
- Refactoring with unchanged behavior: [Reviewer Agent](skills/agents/reviewer.md) plus [Implementer Agent](skills/agents/implementer.md).
- Tests/manual QA: [Tester Agent](skills/agents/tester.md).
- Docs/prompts/changelog: [Documenter Agent](skills/agents/documenter.md).

## Multi-agent handoff

Every agent should leave enough information for the next agent to continue without rereading everything.

Use:

- [Handoff Template](HANDOFF_TEMPLATE.md)
- [Known Decisions](memory/known-decisions.md)
- [Known Issues](memory/known-issues.md)

## Shared rules

- Use [Context Policy](CONTEXT_POLICY.md).
- Use [Router](ROUTER.md) before opening project files.
- Prefer [Before Change Checklist](checklists/before-change.md) and [After Change Checklist](checklists/after-change.md).
- Do not edit legacy duplicate root-level `src/*.c|*.h` files unless the task is specifically about removing or comparing them.
- Do not claim Zsh compatibility beyond what is implemented and tested.
- Do not introduce fake UI/status data; Wisp UI should display real runtime state or show unavailable.
