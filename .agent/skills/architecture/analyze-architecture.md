# Analyze Architecture

## Purpose

Understand Wisp subsystem boundaries before design or refactoring.

## When to use

Use before cross-cutting features, compatibility work, or structural cleanup.

## Inputs

- Task goal
- Subsystem hints
- Relevant docs

## Read first

- [Architecture Map](../../maps/architecture.md)
- [Project Summary](../../memory/project-summary.md)
- [Known Decisions](../../memory/known-decisions.md)
- `docs/ARCHITECTURE.md`
- `docs/REFACTORING_PLAN.md`

## Steps

1. Read architecture docs first.
2. Identify source-of-truth modules.
3. Map data/control flow across touched subsystems.
4. List constraints and risks.
5. Stop before coding unless the target is clear.

## Output

- Boundary summary
- Target modules
- Risk notes

## Verification

The implementation path is narrowed to specific files/modules.

## Next skills

- [Propose Design](propose-design.md)
- [Check Boundaries](check-boundaries.md)
- [Make Plan](../core/make-plan.md)

## Anti-patterns

- Do not analyze every file.
- Do not treat legacy duplicates as active without CMake evidence.
- Do not invent compatibility guarantees.
