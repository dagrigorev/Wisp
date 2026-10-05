# Testing Map

## Use this map when

Use this map when adding, finding, running, stabilizing, or documenting tests and manual QA.

## Start

- [Context Policy](../CONTEXT_POLICY.md)
- [Commands](../memory/commands.md)
- [Find Test Entrypoints](../skills/testing/find-test-entrypoints.md)

## Choose path

### Shell/core/terminal behavior can be automated

- [Add Unit Tests](../skills/testing/add-unit-tests.md)
- Test source: `tests/test_*.c`
- Registration: `tests/CMakeLists.txt`

### Tool behavior can be tested by executable output

- [Add Integration Tests](../skills/testing/add-integration-tests.md)
- Test source: `tests/CMakeLists.txt`
- Target source: `tools`

### GUI/input/pane/rendering behavior is interactive

- [Manual QA](../skills/testing/manual-qa.md)
- Check logs: `%LOCALAPPDATA%\Wisp\logs\wisp.log`

### Regression after a fix/refactor

- [Regression Check](../skills/testing/regression-check.md)
- Prefer the original failing case plus nearest automated tests.

## Finish

- [Run Tests](../skills/testing/run-tests.md)
- [After Change Checklist](../checklists/after-change.md)
- [Write Handoff](../skills/core/write-handoff.md)
