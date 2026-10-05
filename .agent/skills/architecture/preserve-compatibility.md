# Preserve Compatibility

## Purpose

Keep existing Wisp user-visible behavior stable during fixes/refactors.

## When to use

Use when command output, config, themes, man pages, shell syntax, or UI behavior might change.

## Inputs

- Existing behavior
- Proposed change
- Tests/manual QA

## Read first

- [Known Decisions](../../memory/known-decisions.md)
- [Conventions](../../memory/conventions.md)
- [Testing Map](../../maps/testing.md)

## Steps

1. Identify public behavior.
2. Compare old and new expected behavior.
3. Add compatibility tests if possible.
4. Document intentional breaking changes.
5. Update docs/man pages if user-facing.

## Output

- Compatibility notes
- Regression tests/QA
- Docs updates if needed

## Verification

Existing tests and targeted compatibility checks pass.

## Next skills

- [Regression Check](../testing/regression-check.md)
- [Write Handoff](../core/write-handoff.md)

## Anti-patterns

- Do not claim full Zsh compatibility.
- Do not change command output casually.
- Do not break config keys without migration notes.
