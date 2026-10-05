# Make Plan

## Purpose

Create a short, verifiable plan before changing Wisp files.

## When to use

Use after selecting a map/skill and before implementation.

## Inputs

- Task goal
- Target subsystem
- Relevant files
- Verification command or QA steps

## Read first

- [Before Change Checklist](../../checklists/before-change.md)
- [Commands](../../memory/commands.md)
- [Conventions](../../memory/conventions.md)

## Steps

1. State the smallest intended change.
2. List files to inspect/edit.
3. List verification steps.
4. Call out risks such as UI manual QA or ConPTY behavior.
5. Keep the plan short enough for a local model context.

## Output

- Small plan
- File scope
- Verification plan

## Verification

A reviewer can tell what will be changed and how it will be checked.

## Next skills

- [Apply Small Change](apply-small-change.md)
- [Verify Result](verify-result.md)

## Anti-patterns

- Do not plan a rewrite when a patch is enough.
- Do not mix unrelated tasks.
- Do not leave verification undefined.
