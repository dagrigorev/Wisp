# Add Logging

## Purpose

Add targeted diagnostics for Wisp runtime investigation.

## When to use

Use when logs are insufficient to diagnose crashes, ConPTY, input, renderer, or startup issues.

## Inputs

- Issue being diagnosed
- Expected log location
- Relevant source path

## Read first

- [Analyze Logs](../debugging/analyze-logs.md)
- [Conventions](../../memory/conventions.md)
- [Project Summary](../../memory/project-summary.md)

## Steps

1. Find existing logging helpers.
2. Add logs at boundary/failure points only.
3. Avoid logging secrets or huge buffers.
4. Include enough context to connect events.
5. Verify logs appear in `%LOCALAPPDATA%\Wisp\logs\wisp.log`.

## Output

- Targeted logging
- Verification note

## Verification

Run the scenario and confirm useful log entries appear without excessive noise.

## Next skills

- [Trace Execution](../debugging/trace-execution.md)
- [Fix Bug](fix-bug.md)

## Anti-patterns

- Do not spam per-cell/per-character logs in hot paths.
- Do not log private environment values unnecessarily.
- Do not leave temporary debug prints.
