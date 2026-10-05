# Build Map

## Use this map when

Use this map when configure, build, dependency, compiler, linker, test discovery, runtime resource copying, or packaging fails.

## Start

- [Context Policy](../CONTEXT_POLICY.md)
- [Commands](../memory/commands.md)
- [Inspect Task](../skills/core/inspect-task.md)
- [Inspect Build System](../skills/build/inspect-build-system.md)

## Choose path

### Configure fails

- [Configure Build](../skills/build/configure-build.md)
- Check: root `CMakeLists.txt`, generator, MSVC environment.

### Compile or link fails

- [Fix Build Error](../skills/build/fix-build-error.md)
- Check the first meaningful compiler/linker error before cascading errors.

### Runtime files missing from dist

- [Package Artifacts](../skills/build/package-artifacts.md)
- Check: `WISP_DIST_DIR`, root `CMakeLists.txt`, `src/CMakeLists.txt`, `tools/CMakeLists.txt`.

### Tests do not build or register

- [Testing Map](testing.md)
- [Find Test Entrypoints](../skills/testing/find-test-entrypoints.md)
- Check: `tests/CMakeLists.txt`.

## Finish

- [Run Tests](../skills/testing/run-tests.md)
- [Verify Result](../skills/core/verify-result.md)
- [Write Handoff](../skills/core/write-handoff.md)
