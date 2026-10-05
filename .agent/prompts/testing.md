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

# Testing Prompt

## Focus

Use `.agent/maps/testing.md`.

Find existing CTest entrypoints first. Add the narrowest automated test for shell/core/terminal/tool behavior. Use manual QA for GUI, panes, rendering, ConPTY, and keyboard input.

## Commands

Use `.agent/memory/commands.md` for current build/test commands.
