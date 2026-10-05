# Debugging Map

## Use this map when

Use this map when the task is related to crash, wrong behavior, logs, regression, failing tests, bad terminal output, or unknown root cause.

## Start

- [Context Policy](../CONTEXT_POLICY.md)
- [Known Issues](../memory/known-issues.md)
- [Commands](../memory/commands.md)
- [Inspect Task](../skills/core/inspect-task.md)

## Choose path

### Need reproduction

- [Reproduce Issue](../skills/debugging/reproduce-issue.md)

### Logs exist or runtime crash is involved

- [Analyze Logs](../skills/debugging/analyze-logs.md)

### Need source-level trace

- [Trace Execution](../skills/debugging/trace-execution.md)

### Root cause unclear

- [Isolate Root Cause](../skills/debugging/isolate-root-cause.md)

### Root cause known

- [Propose Minimal Fix](../skills/debugging/propose-minimal-fix.md)
- [Fix Bug](../skills/coding/fix-bug.md)

## Wisp starting points

- Shell/runtime: `src/shell`, `src/repl.c`
- Terminal rendering/screen: `src/terminal`
- Keyboard/window/panes: `src/window.cpp`, `src/platform/input.*`
- ConPTY/external shell: `src/platform/pty.*`
- Logs: `%LOCALAPPDATA%\Wisp\logs\wisp.log`
- Tests: `tests`, `ctest --test-dir build --output-on-failure`

## Finish

- [Regression Check](../skills/testing/regression-check.md)
- [Verify Result](../skills/core/verify-result.md)
- [Write Handoff](../skills/core/write-handoff.md)
