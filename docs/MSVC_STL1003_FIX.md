# MSVC STL1003 fix for C translation units

## Problem

Visual Studio 18 / MSVC 14.50 can fail while compiling `.c` files with:

```text
#error: STL1003: Unexpected compiler, expected C++ compiler.
(compiling source file '../../../src/platform/input.c')
```

The failure happens when a C translation unit includes MSVC's STL-backed `<stdbool.h>` header through the toolchain include path. That header pulls in `yvals_core.h`, which is only valid for C++ compilation.

## Fix

WISP now uses `src/core/wisp_bool.h` instead of including `<stdbool.h>` directly.

For MSVC C compilation it defines a small C-compatible `bool`/`true`/`false` shim. For C++ it uses native `bool`. For non-MSVC C compilers it delegates to the standard `<stdbool.h>`.

This keeps C files such as `src/platform/input.c` independent from the C++ STL include chain.
