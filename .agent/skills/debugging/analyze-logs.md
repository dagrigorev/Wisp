# Analyze Logs

## Purpose

Use Wisp logs and failing output to find the first meaningful failure.

## When to use

Use when runtime logs, build logs, crash logs, or test output exists.

## Inputs

- Log path or output
- Scenario
- Timestamp if known

## Read first

- [Commands](../../memory/commands.md)
- [Known Issues](../../memory/known-issues.md)
- [Debugging Map](../../maps/debugging.md)

## Steps

1. Find the first relevant error.
2. Ignore cascading messages until root error is understood.
3. Map messages to subsystem/source files.
4. Summarize evidence.
5. Decide whether tracing or minimal fix is next.

## Output

- Log summary
- Suspected root area
- Next action

## Verification

Evidence points to a source area or a missing reproduction step.

## Next skills

- [Trace Execution](trace-execution.md)
- [Isolate Root Cause](isolate-root-cause.md)
- [Add Logging](../coding/add-logging.md)

## Anti-patterns

- Do not paste huge logs into memory.
- Do not fix the last cascading error first.
- Do not assume logs are complete.
