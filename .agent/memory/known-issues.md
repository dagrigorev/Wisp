# Known Issues

Use this file to track active issues that future agents should know about.

## Format

```text
ID:
Status:
Description:
Affected area:
Reproduction:
Current hypothesis:
Next step:
```

## Issues

### WISP-KI-001 — Legacy duplicate source files

Status:
Open architectural cleanup.

Description:
Root-level duplicate files under `src/` can confuse future edits because the active implementation is the modular tree under `src/core`, `src/shell`, `src/terminal`, and `src/platform`.

Affected area:
Repository structure, refactoring, include resolution.

Reproduction:
Compare `docs/ARCHITECTURE.md` with root-level files such as `src/shell.c`, `src/builtins.c`, `src/screen.c`, and modular equivalents.

Current hypothesis:
They are legacy copies not linked by the current executable target.

Next step:
Only remove or move them in a dedicated refactoring task with a full build/test verification pass.

### WISP-KI-002 — GUI and interactive behavior need manual QA

Status:
Ongoing.

Description:
Automated tests cover many shell/terminal units, but pane splitting, rendering, scrollback, keyboard routing, and ConPTY behavior need Windows-side manual verification.

Affected area:
`src/window.cpp`, `src/repl.c`, `src/terminal`, `src/platform`.

Reproduction:
Build Wisp, run `Wisp.exe`, and execute manual QA from [Manual QA](../skills/testing/manual-qa.md).

Current hypothesis:
CTest is necessary but not sufficient for interactive terminal regressions.

Next step:
For every UI/input/pane/rendering fix, document manual QA steps and results in handoff.

### WISP-KI-003 — Zsh compatibility is incomplete

Status:
Known product limitation.

Description:
The README says it is not honest to claim full Zsh or oh-my-zsh compatibility.

Affected area:
Shell parser, executor, expansion, completion, config/profile compatibility.

Reproduction:
Try non-trivial Zsh syntax or plugins.

Current hypothesis:
Wisp implements a growing shell/runtime but not full Zsh semantics.

Next step:
Implement compatibility incrementally with tests and avoid broad compatibility claims.

### WISP-KI-004 — ConPTY shutdown and resize paths require care

Status:
Needs ongoing verification.

Description:
Architecture notes call out ConPTY shutdown and resize paths as requiring Windows-side behavioral verification.

Affected area:
`src/platform/pty.*`, `src/window.cpp`, terminal resize handling.

Reproduction:
Run external shell through ConPTY, resize window/panes, close sessions, and inspect logs.

Current hypothesis:
Edge cases depend on real Windows pseudo-console behavior.

Next step:
Use [Trace Execution](../skills/debugging/trace-execution.md) and [Manual QA](../skills/testing/manual-qa.md) for related fixes.

### WISP-KI-005 — INPUT_NEXT_TAB / INPUT_PREV_TAB unhandled in WM_KEYDOWN

Status:
Fixed.

Description:
`input_translate()` in `src/platform/input.c` returns `INPUT_NEXT_TAB` (Ctrl+Tab) and `INPUT_PREV_TAB` (Ctrl+Shift+Tab), but the WM_KEYDOWN switch in `src/main.cpp` had no cases for these actions. They fell through to `default: break;`, causing tab switching to be silently dropped.

Affected area:
`src/main.cpp` — WndProc WM_KEYDOWN handler.

Reproduction:
Press Ctrl+Tab or Ctrl+Shift+Tab in Wisp with multiple tabs open. No tab switch occurs.

Current hypothesis:
The actions were defined in the input enum and returned by `input_translate()`, but the switch handler was never extended to cover them.

Next step:
None — fix applied. Manual QA recommended: verify Ctrl+Tab / Ctrl+Shift+Tab cycle tabs correctly with 2+ tabs open.

## Related

- [Debugging Map](../maps/debugging.md)
- [Reproduce Issue](../skills/debugging/reproduce-issue.md)
- [Regression Check](../skills/testing/regression-check.md)
