# Commands

## Setup

Use a Windows environment with MSVC available, for example a Visual Studio Developer PowerShell or Developer Command Prompt.

Initialize user runtime files after building:

```powershell
.\build-run\dist\wshinit.exe
```

## Build

Preferred local workflow:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-and-run-wisp.ps1 -NoRun
```

Build and test in one step:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-and-run-wisp.ps1 -NoRun -RunTests
```

Manual CMake flow:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Test

Preferred:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\build-and-run-wisp.ps1 -NoRun -RunTests
```

Manual:

```powershell
ctest --test-dir build --output-on-failure
```

Run one CTest by name:

```powershell
ctest --test-dir build -R test_name --output-on-failure
```

Useful test names include:

- `test_lexer`
- `test_parser`
- `test_executor`
- `test_builtins`
- `test_history`
- `test_completion`
- `test_screen`
- `test_vt_parser`
- `test_layout`
- `test_repl`
- `test_tool_<tool>_help`

## Run

After build:

```powershell
.\build-run\dist\Wisp.exe
```

Manual CMake output may also place runtime files under the configured `dist` directory inside the build tree.

## Logs

Runtime log path:

```text
%LOCALAPPDATA%\Wisp\logs\wisp.log
```

Fallback log path if `LOCALAPPDATA` is unavailable:

```text
logs\wisp.log
```

## Package

CMake/CPack packaging is configured for ZIP packages.

```powershell
cmake --build build --config Release
cpack --config build\CPackConfig.cmake
```

If the build script creates a different build directory, adapt the `build` path and record it in the handoff.

## Clean

Common cleanup targets:

```powershell
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force .\build-run -ErrorAction SilentlyContinue
```

Do not delete user data under `%APPDATA%\Wisp` or `%LOCALAPPDATA%\Wisp` unless the task explicitly asks for a clean user-runtime test.

## Related

- [Build Map](../maps/build.md)
- [Testing Map](../maps/testing.md)
- [Inspect Build System](../skills/build/inspect-build-system.md)
