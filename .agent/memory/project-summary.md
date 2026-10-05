# Project Summary

## What this project is

Wisp is a Windows-first shell and terminal environment written in C/C++ with CMake.

It combines a native Win32 window, Direct2D/DirectWrite terminal rendering, an internal shell runtime, split panes, VT/ANSI screen handling, scrollback, themes, manual pages, companion Unix-like utilities, and optional external shell hosting through ConPTY.

The project is not a complete Zsh clone. Treat Zsh compatibility as an incremental goal, not as a finished feature.

## Main technologies

- C and C++.
- CMake 3.20+.
- MSVC on Windows.
- Win32 windowing.
- Direct2D and DirectWrite rendering.
- Windows ConPTY for external shell hosting.
- CTest for automated tests.
- PowerShell scripts for local build/run workflow.

## Main entry points

- `CMakeLists.txt` — root build configuration.
- `src/CMakeLists.txt` — `Wisp.exe` target.
- `src/main.cpp` — application startup and wiring.
- `src/window.cpp` / `src/window.h` — Win32 window/UI message handling.
- `src/repl.c` / `src/repl.h` — REPL bridge and line editing.
- `tools/CMakeLists.txt` — companion utilities.
- `tests/CMakeLists.txt` — CTest entrypoints.
- `build-and-run-wisp.ps1` — main local build/run/test script.

## Main modules

- `src/core` — arena, strings, Unicode, logging, path helpers.
- `src/shell` — lexer, parser, executor, expansion, builtins, history, jobs, completion, shell context.
- `src/terminal` — screen buffer, VT parser, layout, Direct2D/DirectWrite renderer, font support.
- `src/platform` — configuration, keyboard input translation, ConPTY lifecycle.
- `tools` — companion commands such as `ls`, `tree`, `ping`, `htop`, `wshinit`, and coreutils-style commands.
- `themes` — built-in theme TOML files.
- `man` — bundled manual pages.
- `docs` — architecture, porting notes, bugfix notes, and refactoring plan.
- `tests` — unit and smoke tests.

## Important constraints

- The active source of truth is the modular tree: `src/core`, `src/shell`, `src/terminal`, `src/platform`.
- Root-level duplicates such as `src/shell.c`, `src/builtins.c`, `src/screen.c`, and similar files are legacy copies and should not be edited for normal feature/bug work.
- The project targets Windows and requires MSVC.
- Do not introduce fake UI/status data. Show real runtime state or unavailable.
- Keep runtime resources copied to `dist`: `config`, `themes`, and `man`.
- Keep local model context small: choose one subsystem before reading source files.

## Current status

Implemented areas include native Win32 UI, terminal renderer, screen buffer, VT parser, scrollback, split panes, shell runtime, history/completion/jobs, ConPTY hosting, config loading, themes, manual pages, logging, companion utilities, and CTest tests.

Areas still requiring care include full Zsh compatibility, parser diagnostics, GUI/manual QA coverage, ConPTY shutdown/resize behavior, shell compatibility edge cases, and cleanup of legacy duplicate source files.

## Related

- [Commands](commands.md)
- [Conventions](conventions.md)
- [Known Decisions](known-decisions.md)
- [Known Issues](known-issues.md)
- [Router](../ROUTER.md)
