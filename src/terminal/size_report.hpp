/* Transliterated from Ghostty src/terminal/size_report.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: `*std.Io.Writer` is `std::string *`, so encode cannot fail and
 * returns void where upstream returns std.Io.Writer.Error!void.
 */

#pragma once
#ifndef WISP_TERMINAL_SIZE_REPORT_HPP
#define WISP_TERMINAL_SIZE_REPORT_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

#include "../vt/size.hpp"

namespace wisp {
namespace terminal {
namespace size_report {

typedef ::wisp::vt::size::CellCountInt CellCountInt;

/* Output formats for terminal size reports written to the PTY. */
enum class Style : uint8_t {
    /* In-band size reports (mode 2048) */
    mode_2048,
    /* XTWINOPS: report text area size in pixels */
    csi_14_t,
    /* XTWINOPS: report cell size in pixels */
    csi_16_t,
    /* XTWINOPS: report text area size in characters */
    csi_18_t,
};

/* Runtime size values used to encode terminal size reports. */
struct Size {
    /* Terminal row count in cells. */
    CellCountInt rows;

    /* Terminal column count in cells. */
    CellCountInt columns;

    /* Width of a single terminal cell in pixels. */
    uint32_t cell_width;

    /* Height of a single terminal cell in pixels. */
    uint32_t cell_height;

    Size() : rows(0), columns(0), cell_width(0), cell_height(0) {}
    Size(CellCountInt r, CellCountInt c, uint32_t cw, uint32_t ch)
        : rows(r), columns(c), cell_width(cw), cell_height(ch) {}
};

inline uint64_t widthPixels(const Size &s) { return (uint64_t)s.columns * (uint64_t)s.cell_width; }

inline uint64_t heightPixels(const Size &s) { return (uint64_t)s.rows * (uint64_t)s.cell_height; }

/* Encode a terminal size report sequence. */
inline void encode(std::string *writer, Style style, const Size &size) {
    char buf[128];
    switch (style) {
    case Style::mode_2048:
        snprintf(buf, sizeof buf, "\x1B[48;%u;%u;%llu;%llut", (unsigned)size.rows, (unsigned)size.columns,
                 (unsigned long long)heightPixels(size), (unsigned long long)widthPixels(size));
        break;

    case Style::csi_14_t:
        snprintf(buf, sizeof buf, "\x1b[4;%llu;%llut", (unsigned long long)heightPixels(size),
                 (unsigned long long)widthPixels(size));
        break;

    case Style::csi_16_t:
        snprintf(buf, sizeof buf, "\x1b[6;%u;%ut", (unsigned)size.cell_height, (unsigned)size.cell_width);
        break;

    case Style::csi_18_t:
        snprintf(buf, sizeof buf, "\x1b[8;%u;%ut", (unsigned)size.rows, (unsigned)size.columns);
        break;
    }
    writer->append(buf);
}

} /* namespace size_report */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SIZE_REPORT_HPP */
