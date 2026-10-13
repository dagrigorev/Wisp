/* Transliterated from Ghostty src/terminal/search.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream re-exports each searcher under a shorter name
 * (`search.Active`, `search.Screen`, ...). The port keeps the full type
 * names from the files themselves and adds these aliases, so both spellings
 * work and the alias is not the only way to name the type.
 *
 * Wisp: `Thread` is omitted. Upstream already excludes it from one of its own
 * build artifacts for exactly the reason it cannot be ported here:
 *
 *     // The search thread is not available in libghostty due to the xev dep
 *     // for now.
 *     pub const Thread = switch (options.artifact) {
 *         .ghostty => @import("search/Thread.zig"),
 *         .lib => void,
 *     };
 *
 * search/Thread.zig is built on libxev — xev.Loop, xev.Async, xev.Completion
 * and xev.Timer — plus BlockingQueue, global.zig and os/main.zig, none of
 * which this port has. Substituting another event loop would make the
 * behaviour that library's rather than upstream's, so this port is in the
 * `.lib` position and the declaration is `void` here too.
 */

#pragma once
#ifndef WISP_VT_SEARCH_HPP
#define WISP_VT_SEARCH_HPP

/* Search functionality for the terminal. */

#include "search/active.hpp"
#include "search/pagelist.hpp"
#include "search/screen.hpp"
#include "search/sliding_window.hpp"
#include "search/terminal.hpp"
#include "search/viewport.hpp"

namespace wisp {
namespace vt {
namespace search {

typedef ActiveSearch   Active;
typedef PageListSearch PageList;
typedef ScreenSearch   Screen;
typedef TerminalSearch Terminal;
typedef ViewportSearch Viewport;

}  // namespace search
}  // namespace vt
}  // namespace wisp

#endif  // WISP_VT_SEARCH_HPP
