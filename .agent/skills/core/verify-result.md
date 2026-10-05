# Verify Result

## Purpose

Check that the Wisp change works and did not break nearby behavior.

## When to use

Use after any code, build, docs, or config change.

## Inputs

- Changed files
- Expected behavior
- Available commands
- Manual QA requirements

## Read first

- [Commands](../../memory/commands.md)
- [After Change Checklist](../../checklists/after-change.md)
- [Testing Map](../../maps/testing.md)

## Steps

1. Run the original failing command/test when available.
2. Run nearest targeted tests.
3. For UI/input/ConPTY behavior, perform manual QA.
4. Record commands and results.
5. Document any skipped verification with reason.

## Output

- Verification summary
- Commands run
- Manual QA result
- Known gaps

## Verification

The original issue is fixed or the remaining gap is explicitly documented.

## Next skills

- [Write Handoff](write-handoff.md)
- [Regression Check](../testing/regression-check.md)

## Anti-patterns

- Do not claim success without verification.
- Do not hide failing tests.
- Do not skip manual QA for interactive UI changes without saying why.
