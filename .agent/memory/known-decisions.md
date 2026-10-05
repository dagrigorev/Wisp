# Known Decisions

Use this file to keep important architectural and implementation decisions.

## Format

```text
YYYY-MM-DD - Decision
Context:
Decision:
Consequences:
Links:
```

## Decisions

### Active modular source tree

Context:
Older root-level files exist directly under `src/`, but `docs/ARCHITECTURE.md` identifies the active implementation as the modular tree.

Decision:
Normal bugfix, feature, and refactoring work should target `src/core`, `src/shell`, `src/terminal`, and `src/platform`, plus `src/main.cpp`, `src/window.cpp`, and `src/repl.c` for wiring.

Consequences:
Do not patch legacy duplicate root-level `src/*.c|*.h|*.cpp` files unless the task is specifically about cleanup or comparison.

Links:
- `docs/ARCHITECTURE.md`
- [Project Summary](project-summary.md)

### Windows/MSVC-only target

Context:
The root `CMakeLists.txt` requires `WIN32` and `MSVC`.

Decision:
Build and verification instructions should assume Windows with MSVC available.

Consequences:
Do not spend time making Linux/macOS builds work unless a future task explicitly changes the product target.

Links:
- `CMakeLists.txt`
- [Commands](commands.md)

### Runtime bundle layout

Context:
The build copies `config`, `themes`, and `man` to the runtime distribution directory.

Decision:
User-facing runtime behavior should work from `dist` / `build-run\dist` and should not depend on source-tree-only resources.

Consequences:
Feature work that adds config, themes, or manual pages must verify the runtime bundle.

Links:
- `CMakeLists.txt`
- `src/CMakeLists.txt`
- [Release Map](../maps/release.md)

### UI must use real data

Context:
The README states that tabs, path bars, status badges, resource meters, and utility output should use active Wisp/session/system state.

Decision:
Do not add dummy/fake values to UI or terminal status surfaces.

Consequences:
If data is unsupported, hide it or show unavailable rather than inventing it.

Links:
- `README.md`
- [Frontend Map](../maps/frontend.md)

## Related

- [Document Decision](../skills/architecture/document-decision.md)
- [Decisions Index](../DECISIONS.md)
