# Trace Execution

## Purpose

Follow Wisp control flow through a small path.

## When to use

Use after reproduction/log analysis identifies a likely subsystem.

## Inputs

- Reproduction
- Suspected files
- Expected flow

## Read first

- [Project Summary](../../memory/project-summary.md)
- [Conventions](../../memory/conventions.md)
- [Read Repository](../core/read-repository.md)

## Steps

1. Start at the entrypoint closest to the behavior.
2. Trace call flow through only necessary functions.
3. Note state transitions and ownership.
4. Identify where actual behavior diverges.
5. Stop when root cause is localized.

## Output

- Trace notes
- Root-cause candidate
- Minimal patch location

## Verification

The failing condition is localized to a function or small module.

## Next skills

- [Isolate Root Cause](isolate-root-cause.md)
- [Propose Minimal Fix](propose-minimal-fix.md)

## Anti-patterns

- Do not trace unrelated subsystems.
- Do not add permanent noisy logs.
- Do not change code while still unsure.
