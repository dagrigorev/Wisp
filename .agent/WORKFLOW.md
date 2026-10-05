# Workflow

## Default flow

1. Read task.
2. Read [Context Policy](CONTEXT_POLICY.md).
3. Use [Router](ROUTER.md).
4. Select one map.
5. Select one primary skill.
6. Read memory before source.
7. Read minimal project files.
8. Make a short plan.
9. Apply a small change.
10. Verify.
11. Write handoff.

## Bugfix flow

1. Use [Debugging Map](maps/debugging.md).
2. Reproduce the issue or document why reproduction is impossible.
3. Check `%LOCALAPPDATA%\Wisp\logs\wisp.log` when runtime behavior is involved.
4. Identify the likely subsystem from [Project Summary](memory/project-summary.md).
5. Read the smallest source slice.
6. Isolate root cause before patching.
7. Apply a minimal fix.
8. Run the original failing command or manual QA.
9. Run targeted tests, then broader tests when practical.
10. Update [Known Issues](memory/known-issues.md) if the bug is not fully closed.

## Feature flow

1. Use [Architecture Map](maps/architecture.md).
2. Check [Known Decisions](memory/known-decisions.md) and [Conventions](memory/conventions.md).
3. Decide the target subsystem: shell, terminal, platform, tools, docs, or tests.
4. Define observable behavior before changing code.
5. Implement in small vertical slices.
6. Add or update tests when behavior can be automated.
7. Add manual QA steps for GUI/terminal behavior.
8. Update docs/man pages when user-facing commands or behavior change.

## Refactoring flow

1. Use [Refactoring Map](maps/refactoring.md).
2. Preserve current behavior and public command surfaces.
3. Prefer one subsystem per change.
4. Avoid mixing formatting cleanup with semantic changes.
5. Add characterization tests before risky movement.
6. Run regression tests and relevant manual QA.
7. Document any architectural decision that changes boundaries.

## Review flow

1. Use [Read Repository](skills/core/read-repository.md) only for the relevant area.
2. Check changed files against [Conventions](memory/conventions.md).
3. Verify boundaries, tests, build impact, and risk.
4. Report actionable findings only.
5. Finish with [Review Done Checklist](checklists/review-done.md).

## Release flow

1. Use [Release Map](maps/release.md).
2. Verify clean configure/build/test flow.
3. Verify runtime resources in `dist`.
4. Package artifacts.
5. Update changelog or notes.
6. Write handoff with exact commands and outputs.

## Links

- [Router](ROUTER.md)
- [Skills Index](skills/index.md)
- [Before Change Checklist](checklists/before-change.md)
- [After Change Checklist](checklists/after-change.md)
