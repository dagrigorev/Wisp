# Read Repository

## Purpose

Find the minimal Wisp source area needed for the task.

## When to use

Use when the needed files are not obvious after reading the router and map.

## Inputs

- Task summary
- Selected map
- Likely subsystem

## Read first

- [Context Policy](../../CONTEXT_POLICY.md)
- [Project Summary](../../memory/project-summary.md)
- [Known Decisions](../../memory/known-decisions.md)
- `docs/ARCHITECTURE.md`

## Steps

1. Identify task type and subsystem.
2. Prefer memory and architecture docs before source.
3. Open only CMake/docs/source files directly related to the subsystem.
4. Stop when target files and verification path are known.
5. Summarize what was found before deeper reading.

## Output

- Minimal file list
- Subsystem summary
- Known verification path

## Verification

Confirm that the selected files include the code path to be changed and a nearby test or QA path.

## Next skills

- [Make Plan](make-plan.md)
- [Apply Small Change](apply-small-change.md)
- [Write Handoff](write-handoff.md)

## Anti-patterns

- Do not recursively read the whole repository.
- Do not inspect build outputs or binaries.
- Do not edit legacy duplicate `src` files unless explicitly required.
