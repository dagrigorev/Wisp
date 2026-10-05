# Glossary

Keep terms short and factual. Add new terms only after confirming them in repository files.

## Wisp

Windows-first shell and terminal environment built with C/C++, CMake, Win32, Direct2D/DirectWrite, and ConPTY.

## Built-in shell

The internal Wisp shell runtime implemented mainly under `src/shell` and connected through `src/repl.c`.

## ConPTY

Windows pseudo-console API used for hosting external shells. Wisp platform code lives under `src/platform/pty.*`.

## Terminal subsystem

Screen buffer, VT parser, layout, font, and renderer code under `src/terminal`.

## Companion utilities

Standalone tools built from `tools`, copied into the runtime `dist` folder, and tested through CTest help/smoke tests.

## Runtime resources

`config`, `themes`, and `man` directories copied to `dist` during build.

## Legacy duplicate sources

Root-level duplicate files under `src/*.c|*.h|*.cpp` that are not the active modular source of truth according to `docs/ARCHITECTURE.md`.

## Related

- [Project Summary](memory/project-summary.md)
- [Architecture Map](maps/architecture.md)
