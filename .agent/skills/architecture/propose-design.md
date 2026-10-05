# Propose Design

## Purpose

Design a small Wisp change that fits existing boundaries.

## When to use

Use before implementing non-trivial features or changes touching multiple modules.

## Inputs

- Feature/change goal
- Current architecture summary
- Compatibility constraints

## Read first

- [Analyze Architecture](analyze-architecture.md)
- [Conventions](../../memory/conventions.md)
- [Known Decisions](../../memory/known-decisions.md)

## Steps

1. Define behavior and non-goals.
2. Choose module ownership.
3. Define interfaces/data flow.
4. Identify tests/manual QA.
5. Document tradeoffs if durable.

## Output

- Small design proposal
- File/module plan
- Verification plan

## Verification

Design can be implemented by a small sequence of changes.

## Next skills

- [Check Boundaries](check-boundaries.md)
- [Implement Feature](../coding/implement-feature.md)
- [Document Decision](document-decision.md)

## Anti-patterns

- Do not design large frameworks without need.
- Do not move shell semantics into renderer/platform.
- Do not introduce fake UI state.
