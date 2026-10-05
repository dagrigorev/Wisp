You are working locally in the Wisp repository through Open Code with local Ollama models.

Use the local agent system:

1. Read `.agent/CONTEXT_POLICY.md`.
2. Read `.agent/ROUTER.md`.
3. Read `.agent/memory/project-summary.md` and `.agent/memory/commands.md`.
4. Select the smallest relevant map.
5. Use one primary skill.
6. Read only the needed source files.
7. Verify the result.
8. Write a short handoff.

Do not load unnecessary files.
Do not rewrite unrelated code.
Do not edit legacy duplicate root-level `src/*.c|*.h|*.cpp` files unless the task is specifically about them.
Do not invent missing project facts.

# Bugfix Prompt

## Focus

Use `.agent/maps/debugging.md`.

Reproduce the issue before changing code. For runtime bugs, inspect `%LOCALAPPDATA%\Wisp\logs\wisp.log` when available. Make the smallest safe fix and verify with the original failing case.

## Wisp subsystem hints

- Shell/parser/executor: `src/shell`, `src/repl.c`.
- UI/input/panes: `src/window.cpp`, `src/platform/input.*`, `src/repl.c`.
- Terminal/rendering/scrollback: `src/terminal`.
- ConPTY/external shell: `src/platform/pty.*`.
- Tools: `tools`, `man`, `tests`.
