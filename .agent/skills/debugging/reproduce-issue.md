# Reproduce Issue

## Purpose

Create a reliable Wisp reproduction before fixing.

## When to use

Use for crashes, wrong output, failed tests, UI/input bugs, and tool regressions.

## Inputs

- Issue report
- Environment
- Commands or user actions

## Read first

- [Debugging Map](../../maps/debugging.md)
- [Commands](../../memory/commands.md)
- [Known Issues](../../memory/known-issues.md)

## Steps

1. Run the reported command/test/action.
2. Capture exact observed behavior.
3. Capture logs if runtime is involved.
4. Reduce to the smallest repeatable case.
5. Classify subsystem.

## Output

- Reproduction steps
- Observed vs expected result
- Likely subsystem

## Verification

The issue can be repeated or a clear reason for non-reproduction is recorded.

## Next skills

- [Analyze Logs](analyze-logs.md)
- [Trace Execution](trace-execution.md)
- [Fix Bug](../coding/fix-bug.md)

## Anti-patterns

- Do not patch before reproduction if reproduction is practical.
- Do not rely on vague descriptions.
- Do not skip logs for runtime crashes.
