# Apply Small Change

## Purpose

Make the smallest safe code or docs change for the selected Wisp task.

## When to use

Use once the plan and target files are clear.

## Inputs

- Plan
- Target files
- Conventions
- Expected behavior

## Read first

- [Conventions](../../memory/conventions.md)
- [Before Change Checklist](../../checklists/before-change.md)
- [Known Decisions](../../memory/known-decisions.md)

## Steps

1. Open only the target files.
2. Modify the minimal code path.
3. Keep behavior outside the target unchanged.
4. Update tests/docs only when relevant.
5. Record exact changed files for handoff.

## Output

- Small patch
- Changed file list
- Notes for verification

## Verification

The diff is limited to the planned area and can be verified by the planned command or QA.

## Next skills

- [Verify Result](verify-result.md)
- [Run Tests](../testing/run-tests.md)
- [Write Handoff](write-handoff.md)

## Anti-patterns

- Do not reformat entire files.
- Do not update dependencies unnecessarily.
- Do not introduce fake UI data.
