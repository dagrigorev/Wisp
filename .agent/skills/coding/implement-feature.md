# Implement Feature

## Purpose

Add a Wisp feature in a small, testable vertical slice.

## When to use

Use when behavior is intentionally being added or extended.

## Inputs

- Feature goal
- User-visible behavior
- Target subsystem
- Compatibility constraints

## Read first

- [Architecture Map](../../maps/architecture.md)
- [Known Decisions](../../memory/known-decisions.md)
- [Conventions](../../memory/conventions.md)
- [Commands](../../memory/commands.md)

## Steps

1. Define the observable behavior.
2. Choose one subsystem and boundary.
3. Add the minimal implementation.
4. Update tests or manual QA.
5. Update man/docs when user-facing commands change.
6. Verify build and behavior.

## Output

- Implemented feature
- Tests or QA notes
- Docs/man updates if needed

## Verification

Run targeted tests and, for UI/terminal behavior, manual QA. Confirm runtime bundle when resources are added.

## Next skills

- [Add Unit Tests](../testing/add-unit-tests.md)
- [Manual QA](../testing/manual-qa.md)
- [Write Handoff](../core/write-handoff.md)

## Anti-patterns

- Do not add dummy UI data.
- Do not over-design broad frameworks.
- Do not silently change existing command behavior without compatibility notes.
