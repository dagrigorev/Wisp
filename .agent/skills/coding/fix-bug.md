# Fix Bug

## Purpose

Fix a confirmed Wisp bug with the smallest safe change.

## When to use

Use after reproduction or clear failing test/log identifies a defect.

## Inputs

- Reproduction steps
- Observed vs expected behavior
- Target subsystem
- Relevant logs/tests

## Read first

- [Debugging Map](../../maps/debugging.md)
- [Known Issues](../../memory/known-issues.md)
- [Commands](../../memory/commands.md)
- [Conventions](../../memory/conventions.md)

## Steps

1. Confirm the bug path.
2. Locate the smallest responsible function/module.
3. Write or identify a failing test if practical.
4. Patch only the root cause.
5. Run the original failing case.
6. Run nearest regression tests.
7. Document manual QA for UI/runtime behavior.

## Output

- Minimal fix
- Root-cause explanation
- Verification result

## Verification

Run original reproduction. Then run targeted CTest or manual QA from the selected map.

## Next skills

- [Regression Check](../testing/regression-check.md)
- [Write Handoff](../core/write-handoff.md)

## Anti-patterns

- Do not patch without reproduction when reproduction is possible.
- Do not mask errors by ignoring return values.
- Do not change unrelated behavior.
