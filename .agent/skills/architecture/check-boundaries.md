# Check Boundaries

## Purpose

Ensure a Wisp change stays in the correct subsystem.

## When to use

Use during feature, bugfix, refactor, or review.

## Inputs

- Changed files or planned files
- Subsystem purpose
- Relevant tests

## Read first

- [Conventions](../../memory/conventions.md)
- [Known Decisions](../../memory/known-decisions.md)
- [Architecture Map](../../maps/architecture.md)

## Steps

1. Check that shell code is not taking UI dependencies.
2. Check terminal code is not taking shell semantics.
3. Check platform code owns Win32/ConPTY details.
4. Check tools have man/help/test updates when user-facing.
5. Flag boundary violations early.

## Output

- Boundary review
- Required follow-up changes

## Verification

No obvious module ownership violation remains.

## Next skills

- [Preserve Compatibility](preserve-compatibility.md)
- [Review Done Checklist](../../checklists/review-done.md)

## Anti-patterns

- Do not approve convenience coupling that makes tests harder.
- Do not move logic into `main.cpp` as a shortcut.
- Do not ignore runtime resources.
