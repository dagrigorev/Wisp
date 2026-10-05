# Backend Map

## Use this map when

Use this map when the task affects Wisp shell semantics, command execution, builtins, history, jobs, completion, environment, config parsing, or companion utilities.

## Start

- [Context Policy](../CONTEXT_POLICY.md)
- [Project Summary](../memory/project-summary.md)
- [Conventions](../memory/conventions.md)
- [Inspect Task](../skills/core/inspect-task.md)

## Choose path

### Shell parser/executor/expansion bug

- [Reproduce Issue](../skills/debugging/reproduce-issue.md)
- [Trace Execution](../skills/debugging/trace-execution.md)
- [Fix Bug](../skills/coding/fix-bug.md)
- Start source: `src/shell/lexer.*`, `src/shell/parser.*`, `src/shell/executor.*`, `src/shell/expand.*`

### Builtin/history/completion/jobs behavior

- [Analyze Logs](../skills/debugging/analyze-logs.md)
- [Fix Bug](../skills/coding/fix-bug.md)
- Start source: `src/shell/builtins.*`, `src/shell/history.*`, `src/shell/completion.*`, `src/shell/jobs.*`, `src/repl.c`

### Config or external shell hosting

- [Debugging Map](debugging.md)
- [Check Boundaries](../skills/architecture/check-boundaries.md)
- Start source: `src/platform/config.*`, `src/platform/pty.*`

### Companion utility feature or bug

- [Implement Feature](../skills/coding/implement-feature.md)
- [Add Unit Tests](../skills/testing/add-unit-tests.md)
- Start source: `tools`, `man`, `tests/CMakeLists.txt`

## Finish

- [Run Tests](../skills/testing/run-tests.md)
- [Regression Check](../skills/testing/regression-check.md)
- [Write Handoff](../skills/core/write-handoff.md)
