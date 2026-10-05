# Isolate Root Cause

## Purpose

Separate symptoms from the actual Wisp defect.

## When to use

Use when multiple files or errors could explain the issue.

## Inputs

- Reproduction
- Logs
- Trace notes

## Read first

- [Trace Execution](trace-execution.md)
- [Known Issues](../../memory/known-issues.md)
- [Conventions](../../memory/conventions.md)

## Steps

1. List possible causes.
2. Eliminate causes using tests, logs, or code evidence.
3. Identify the smallest incorrect assumption/state.
4. Choose the minimal fix location.
5. Record why other causes were rejected.

## Output

- Root-cause statement
- Rejected alternatives
- Fix target

## Verification

Root cause explains the observed behavior and predicts the fix.

## Next skills

- [Propose Minimal Fix](propose-minimal-fix.md)
- [Fix Bug](../coding/fix-bug.md)

## Anti-patterns

- Do not guess when a test/log can decide.
- Do not confuse workaround with root cause.
- Do not broaden the patch area.
