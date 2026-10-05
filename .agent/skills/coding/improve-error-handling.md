# Improve Error Handling

## Purpose

Make Wisp failures explicit and diagnosable without hiding real errors.

## When to use

Use when errors are swallowed, crashes are unclear, or user-facing diagnostics are weak.

## Inputs

- Failure mode
- Affected subsystem
- Expected user/log behavior

## Read first

- [Conventions](../../memory/conventions.md)
- [Analyze Logs](../debugging/analyze-logs.md)
- [Commands](../../memory/commands.md)

## Steps

1. Locate the failing boundary.
2. Return or log meaningful error context.
3. Preserve existing success behavior.
4. Avoid broad exception swallowing.
5. Add tests or manual QA for the failure path.

## Output

- Improved diagnostics
- No hidden errors
- Verification result

## Verification

Trigger the failure path if possible and confirm diagnostics/logging are useful.

## Next skills

- [Add Logging](add-logging.md)
- [Regression Check](../testing/regression-check.md)

## Anti-patterns

- Do not convert failures into silent success.
- Do not show raw internal noise to users unnecessarily.
- Do not log secrets/local private data.
