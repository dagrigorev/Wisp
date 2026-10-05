# Environment

## Local AI

- Runtime: Ollama
- Coding tool: Open Code

## Expected local workflow

Agents should work locally, inspect files directly, and avoid unnecessary large context loading.

## Project-specific environment

- OS target: Windows.
- Compiler requirement: MSVC.
- Build system: CMake.
- Test runner: CTest.
- Runtime graphics/UI stack: Win32, Direct2D, DirectWrite.
- External shell hosting: Windows ConPTY.
- Main build helper: `build-and-run-wisp.ps1`.
- Typical runnable output: `build-run\dist`.
- Runtime config: `%APPDATA%\Wisp\Wisp.toml`.
- Runtime logs: `%LOCALAPPDATA%\Wisp\logs\wisp.log`.

## Local model guidance

- Prefer exact file paths and commands over broad repository scans.
- Keep each pass focused on one subsystem.
- Summarize findings before opening another subsystem.
- Do not read generated build directories, binaries, screenshots, or CPack output unless the task specifically requires it.

## Related

- [Agents](../AGENTS.md)
- [Context Policy](../CONTEXT_POLICY.md)
- [Commands](commands.md)
