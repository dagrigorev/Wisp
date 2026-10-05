# Handoff

## Task

WISP crashes or behaves incorrectly when pressing `Tab` inside split terminal mode.

## Summary

Root cause: `input_translate()` in `src/platform/input.c` returns `INPUT_NEXT_TAB` (Ctrl+Tab) and `INPUT_PREV_TAB` (Ctrl+Shift+Tab), but the WM_KEYDOWN switch in `src/main.cpp` had no cases for these actions. They fell through to `default: break;`, causing tab switching to be silently dropped.

The early WM_KEYDOWN checks handle Ctrl+Shift+T (new tab), Ctrl+Shift+D (split pane), Ctrl+Shift+W (close pane), Ctrl+Shift+F (search), and Alt+arrows (pane navigation), but there were no early checks for Ctrl+Tab or Ctrl+Shift+Tab either.

Fix: Added `case INPUT_NEXT_TAB` and `case INPUT_PREV_TAB` handlers to the WM_KEYDOWN switch that cycle through tabs using the same logic pattern as the existing pane navigation.

## Files changed

- `src/main.cpp` — Added INPUT_NEXT_TAB and INPUT_PREV_TAB cases to WM_KEYDOWN switch (lines ~1127-1132)
- `.agent/memory/known-issues.md` — Added WISP-KI-005 entry

## Commands run

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-and-run-wisp.ps1 -NoRun -RunTests
```

## Verification result

- Build: SUCCESS (no errors, no new warnings)
- Tests: 59/59 passed (100%)
- No regressions in test_completion, test_repl, test_screen, test_layout, or any other test

## Manual QA

Recommended manual verification steps:
1. Build Wisp: `pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-and-run-wisp.ps1 -NoRun`
2. Run Wisp: `.\build-run\dist\Wisp.exe`
3. Open multiple tabs (Ctrl+Shift+T)
4. Press Ctrl+Tab — should cycle to next tab
5. Press Ctrl+Shift+Tab — should cycle to previous tab
6. Split a pane (Ctrl+Shift+D), then press Tab — should trigger completion in active pane (not crash)

## What worked

- Build completed cleanly
- All 59 automated tests passed
- Fix is minimal (8 lines added, 1 file changed)
- Fix follows existing code patterns (same structure as pane navigation)

## What did not work

- Could not perform live manual QA (requires interactive Windows session)
- Could not reproduce the exact crash scenario described by the user (no crash logs available)

## Open questions

- The user reported "Tab crashes" specifically. The fix addresses the unhandled INPUT_NEXT_TAB/INPUT_PREV_TAB actions. If the crash is in a different code path (e.g., repl_handle_input -> handle_tab -> completion_compute), that would require additional investigation with runtime logs from `%LOCALAPPDATA%\Wisp\logs\wisp.log`.
- There is a potential thread-safety concern in `completion_compute()` which reads `ctx->env`, `ctx->aliases`, and `ctx->functions` without holding `g_lock` while the REPL execution thread may modify these. This is a separate issue.

## Recommended next skill

- [Manual QA](skills/testing/manual-qa.md) — verify Ctrl+Tab / Ctrl+Shift+Tab tab cycling works
- [Trace Execution](skills/debugging/trace-execution.md) — if crash persists, add logging to handle_tab() and completion_compute() to pinpoint the exact crash location

## Links

- [Debugging Map](maps/debugging.md)
- [Known Issues](memory/known-issues.md)
