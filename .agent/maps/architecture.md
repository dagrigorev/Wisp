# Architecture Map

## Use this map when

Use this map when the task affects module boundaries, compatibility, public command behavior, runtime layout, long-term design, or cross-subsystem changes.

## Start

- [Context Policy](../CONTEXT_POLICY.md)
- [Project Summary](../memory/project-summary.md)
- [Known Decisions](../memory/known-decisions.md)
- [Analyze Architecture](../skills/architecture/analyze-architecture.md)

## Choose path

### Need current design

- [Analyze Architecture](../skills/architecture/analyze-architecture.md)
- Read first: `docs/ARCHITECTURE.md`, `docs/REFACTORING_PLAN.md`

### Need new feature design

- [Propose Design](../skills/architecture/propose-design.md)
- [Check Boundaries](../skills/architecture/check-boundaries.md)

### Need compatibility or public behavior preserved

- [Preserve Compatibility](../skills/architecture/preserve-compatibility.md)

### Need decision record

- [Document Decision](../skills/architecture/document-decision.md)

## Wisp boundary reminders

- Shell semantics stay in `src/shell`.
- Terminal/screen/rendering stays in `src/terminal`.
- Windows-specific services stay in `src/platform`.
- UI/wiring stays in `src/main.cpp`, `src/window.cpp`, and `src/repl.c`.
- Companion command behavior belongs in `tools`, `man`, and tests.

## Finish

- [Make Plan](../skills/core/make-plan.md)
- [Implement Feature](../skills/coding/implement-feature.md)
- [Write Handoff](../skills/core/write-handoff.md)
