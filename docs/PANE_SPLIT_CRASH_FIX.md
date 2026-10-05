# Pane split crash/exit fix

## Symptom

Pressing `Ctrl+Shift+D` closed WISP or made it look like the process crashed.

## Root cause

On Windows, the message loop calls `TranslateMessage()` before `DispatchMessageW()`.
For `Ctrl+D`, Windows can still post a following `WM_CHAR` with control character
`0x04` (EOT), even if `WM_KEYDOWN` was already handled as the pane split hotkey.

The REPL correctly treats `Ctrl+D` on an empty line as EOF/exit. So the sequence was:

1. `WM_KEYDOWN Ctrl+Shift+D` creates a pane.
2. The following `WM_CHAR 0x04` is delivered to the newly active empty pane.
3. The new REPL interprets it as EOF and exits.

## Fix

Pane-management hotkeys now set `g_suppress_char = true` before returning:

- `Ctrl+Shift+D`
- `Ctrl+Shift+W`
- `Alt+Left/Up`
- `Alt+Right/Down`

The next synthetic `WM_CHAR` is swallowed and never reaches the REPL/PTY.

Also fixed pane initialization to preserve the target pane rectangle across
`memset()`, instead of temporarily forcing new panes to `800x600`.

## QA

1. Start WISP.
2. Press `Ctrl+Shift+D` several times.
3. WISP must stay open and create panes.
4. Type in each pane and switch using `Alt+Left/Right`.
5. Press plain `Ctrl+D` on an empty line: this should still exit the active REPL as before.
