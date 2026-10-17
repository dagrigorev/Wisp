# Porting conventions

Wisp is a Windows C++ port of [Ghostty](https://github.com/ghostty-org/ghostty). The
port is an **exact line-by-line transliteration** of the upstream Zig, not a
reimplementation. This document is the contract; it exists so that anyone — or any
agent — picking up a file ports it the same way the rest of the tree was ported.

## The reference

Upstream is pinned to revision `b32f20f3e8d25bb925ec545c54498e93518e7ced`
(2026-09-18), the revision recorded at the top of `GHOSTTY_PORT_LEDGER.md`. Upstream
`main` has drifted well past it, so working against `main` would silently stop matching
the files already ported.

The clone is kept **outside the repository**, in the session scratchpad, and is
read-only. Never commit it, never modify it.

If the clone is missing, re-clone `https://github.com/ghostty-org/ghostty` and
`git checkout b32f20f3e8d25bb925ec545c54498e93518e7ced`. If the revision itself is ever
lost, recover it by fingerprinting the ledger's per-file line counts (`Terminal.zig`
16541, `Screen.zig` 12487, `PageList.zig` 20542) against the history.

## What "exact" means

The upstream tests define it. Every upstream test is ported with **the same name and the
same assertions**. A port is finished when its tests are ported and passing — not when
the implementation looks right. If a test cannot be ported, say so explicitly in a
comment rather than dropping it silently.

Keep upstream's declaration order, function order, and control flow. Resist tidying:
a reordered function or a collapsed branch makes the next diff against upstream
unreadable.

Where a decision departs from upstream, mark it with a comment beginning `Wisp:` and
state why. Those comments are the audit trail for everything that is not a mechanical
mapping.

## Language mapping

| Zig | C++ |
|---|---|
| `!T` error union | `bool` or an error-enum return, with the value in an out-param |
| `?T` | `Maybe<T>` (`.has` / `.value` / `Maybe<T>::none()`), or a `has_x` flag beside the field |
| slice `[]T` | pointer + length pair |
| `union(enum)` | tag enum + payload fields |
| `*std.Io.Writer` | `std::string *` — writes cannot fail, so no error path |
| `log.debug` / `log.warn` etc. | a comment; there is no logging in the port |
| `defer` / `errdefer` | an RAII guard |
| `+|` / `-|` saturating ops | the saturating helpers |
| `lib.Enum(lib.target, ...)` | a plain `enum class` |
| `std.testing.allocator` leak check | `zigstd::testing_state().live`, asserted by a trailing `zz_Wisp_no_leaks` test |

Naming: upstream `CamelCase.zig` files become `snake_case.hpp`; types keep their
upstream names. Include guards are `WISP_<DIR>_<FILE>_HPP`.

## Build configuration

These upstream features are disabled in Wisp:

```
glyph_protocol      = false
tmux_control_mode   = false
```

Code and tests behind them are omitted, each with a `Wisp:` note saying which flag
gated it. Do not port them "for completeness" — they pull in dependencies the port does
not have.

`kitty_graphics` was disabled until 2026-10-16. The subsystem is now ported and the
flag is on, so a `Wisp:` note citing it is stale and the gated code it describes should
be restored rather than left out. Sixel is original work, not a port: upstream has no
Sixel implementation, so `src/terminal/sixel.hpp` is written to DEC STD 070 and says so.

One consequence worth stating, because it has already caused rework: when a gated
upstream file is finally ported, **search the tree for it before porting it.** A file
omitted for a flag may already be in the port, transliterated inside whichever file
needed it first.

`terminal/kitty/graphics_render.zig` is the worked example, and it cost a broken build.
Its 27-line `Placement` struct was already transliterated inside
`graphics_unicode.hpp`, which needed it as a return type, behind a
`__has_include("graphics_render.hpp")` check that would prefer a sibling port if one
ever appeared. The ledger said `n/a`, nobody looked, and it was "ported" a second time
as a new file — in a different namespace to the one the check expected, which flipped
the guard, dropped the inline copy and broke the build in a translation unit that an
incremental build did not recompile. The lesson is in two parts: grep first, and when a
file provides a type another file already declares a fallback for, match the namespace
the fallback expects.

Separately, when the real file does arrive, check whether anything paraphrased it in the
meantime. The Direct2D renderer re-derived that same struct by hand as
`ScreenImagePlacement` while the flag was off. A paraphrase of an upstream type is a
transliteration debt, not a design choice.

Tests are compiled with `WISP_IS_TEST=1` and `WISP_SLOW_RUNTIME_SAFETY=1`.

## Traps that have already cost time

**`ASSERT_STR_EQ` dangles on a temporary.** It expands to several statements, so a
temporary `std::string` argument is destroyed before the comparison and you read freed
memory — which happens to pass in a build whose freed memory still holds the old bytes,
and fails in Debug against the `0xCC` fill. This bit three times, including once on the
*expected* argument. Bind through the local `EXPECT_STR` macro, or assign a named local
first. Both arguments, every time.

```cpp
#define EXPECT_STR(expected, actual)                                                  \
    do {                                                                              \
        const std::string _actual = (actual);                                         \
        ASSERT_STR_EQ(expected, _actual.c_str());                                     \
    } while (0)
```

**`"\x1bc"` does not compile as intended.** C++ hex escapes are greedy, so that is one
out-of-range character. Write `"\x1b" "c"`.

**`windows.h` defines `RGB`**, which shadows `terminal::RGB`. `#undef RGB` before
including port headers.

**Do not `memset` a struct containing C++ objects.** Reset the fields explicitly;
a `std::vector` member zeroed in place corrupts the allocator.

**Byte lengths in tests are easy to get wrong** when the string contains escapes or an
embedded NUL — count them, do not eyeball them.

**Zig's `undefined` and `unreachable` carry guarantees C++ drops.** This has been caught
three times, always by MSVC `C4701`, and always in code that looked like a faithful
transliteration:

- A variable declared `= undefined` and assigned in every path upstream can reach is
  genuinely indeterminate in C++ if the port's control flow lets one path through.
  Value-initialize it, and note in a `Wisp:` comment that upstream relies on the
  invariant. That is strictly more defined than upstream and identical whenever the
  invariant holds.
- A labeled block that *yields* a value on every path but one, where that path is
  `unreachable`, becomes a plain local plus an `assert`. `NDEBUG` drops the assert, so
  the read of the unset value becomes reachable in exactly the build where it matters.

Treat a `C4701` in ported code as a real finding, never as noise to silence.

**Zig evaluates a `for (0..n)` range once.** Transliterating it into a C++ `for` whose
condition re-reads a field the body mutates changes the trip count. This silently halved
a loop in `search/pagelist.zig`'s tests — `page.size.rows` was re-read while `grow()`
incremented it — and the test failed far from the cause. Snapshot the bound into a
`const` before the loop. A Zig `while` with the same shape *is* re-evaluating, so leave
those alone.

**`test_helpers.h`'s `ASSERT_EQ` casts both sides to `long`**, which truncates a 64-bit
pointer. Compare pointers with `ASSERT_TRUE(a == b)`.

## Verifying

Configure and build (NMake, real Debug config), then run the suite:

```
cmake -S . -B build-ci -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ci --target <target>
ctest --test-dir build-ci -C Debug
```

With a multi-config generator, `ctest` without `-C Debug` reports every test as
"Not Run".

When passing a build type to CMake from PowerShell, **quote the whole argument**:
`"-DCMAKE_BUILD_TYPE=$Configuration"`. A bare dash-leading argument is passed to a
native command literally, variable unexpanded — which is how both CI matrix legs once
compiled identically and how released binaries shipped unoptimized.

### An incremental build is not evidence

Before claiming a change is clean, build it into a **fresh tree**:

```
cmake -S . -B build-fresh -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-fresh --target all
ctest --test-dir build-fresh -C Debug
```

This is header-only in large part, so a change to one header is only seen by the
translation units that happen to get recompiled. A green incremental build over a warm
`build-ci` says nothing about the ones that did not. Twice in one day it said the wrong
thing: it hid the `C4702` in `zigstd/allocator.hpp`, which was reported and dismissed as
unreproducible, and it hid a hard compile error in `kitty/graphics_unicode.hpp` that
failed CI on both matrix legs minutes later. CI always builds cold. Match it before
pushing.

## Ledger

`GHOSTTY_PORT_LEDGER.md` is generated by `tools/gen_port_ledger.py`. Only **status**
cells are edited by hand; they are carried forward when the ledger is regenerated. The
status cells currently lag reality — several files listed `todo` are ported and tested.
