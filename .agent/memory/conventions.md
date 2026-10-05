# Conventions

## Code style

- Keep changes small and localized.
- Prefer explicit error handling over silent failure.
- Preserve existing C/C++ style in the touched file.
- Avoid broad formatting-only edits mixed with behavior changes.
- Keep Windows API usage behind `src/platform` unless the UI or main wiring needs it directly.

## Naming

- Follow existing `wisp_*` naming patterns in C modules.
- Keep module names aligned with their folder responsibility: `core`, `shell`, `terminal`, `platform`.
- Utility commands should match their executable name and manual page where applicable.

## Architecture

- Active source of truth is the modular tree under `src/core`, `src/shell`, `src/terminal`, and `src/platform`.
- Shell semantics should not depend on renderer or Win32 GUI details.
- Terminal rendering/screen code should not depend on shell language semantics.
- Platform-specific behavior belongs in `src/platform`.
- `src/main.cpp`, `src/window.cpp`, and `src/repl.c` are wiring/bridge areas; avoid moving large business logic there.
- Runtime resources must remain available in `dist`: `config`, `themes`, `man`.

## Testing

- Add or update CTest tests for shell/parser/executor/core/terminal behavior when possible.
- Use manual QA for GUI, panes, input routing, ConPTY, rendering, and interactive terminal behavior.
- Companion utilities should keep stable `--help` output and smoke-test coverage.
- When fixing a regression, add the narrowest automated test that would fail before the fix.

## Commits

- One logical change per commit.
- Commit message should name the subsystem: `shell`, `terminal`, `platform`, `tools`, `tests`, `docs`, or `build`.
- Include verification commands in the commit body or handoff when practical.

## Related

- [Project Summary](project-summary.md)
- [Known Decisions](known-decisions.md)
- [Before Commit Checklist](../checklists/before-commit.md)
