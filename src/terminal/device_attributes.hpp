/* Transliterated from Ghostty src/terminal/device_attributes.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: *std.Io.Writer is a std::string appended to, which cannot fail.
 */

#pragma once
#ifndef WISP_TERMINAL_DEVICE_ATTRIBUTES_HPP
#define WISP_TERMINAL_DEVICE_ATTRIBUTES_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

namespace wisp {
namespace terminal {
namespace device_attributes {

/* The device attribute request type (CSI c). */
enum class Req : uint8_t {
    primary,   /* Blank */
    secondary, /* > */
    tertiary,  /* = */
};

/* Conformance level reported as the first parameter (Pp) in the
 * primary device attributes (DA1) response. */
enum class ConformanceLevel : uint16_t {
    /* VT100-series have per-model values. */
    vt100 = 1,
    vt132 = 4,
    vt102 = 6,
    vt131 = 7,
    vt125 = 12,

    /* VT200+ use 60 + decTerminalID/100. */
    /* Level 2 conformance (VT200 series, e.g. VT220, VT240). */
    level_2 = 62,
    /* Level 3 conformance (VT300 series, e.g. VT320, VT340). */
    level_3 = 63,
    /* Level 4 conformance (VT400 series, e.g. VT420). */
    level_4 = 64,
    /* Level 5 conformance (VT500 series, e.g. VT510, VT520, VT525). */
    level_5 = 65,

    /* Wisp: upstream's aliases are decls; here they are enumerators with
     * the same values. */
    vt101 = vt100,
    vt220 = level_2,
    vt240 = level_2,
    vt320 = level_3,
    vt340 = level_3,
    vt420 = level_4,
    vt510 = level_5,
    vt520 = level_5,
    vt525 = level_5,
};

/* Terminal type identifier reported as the Pp parameter in the
 * secondary device attributes (DA2) response. Values correspond
 * to the decTerminalID resource in xterm. */
enum class DeviceType : uint16_t {
    vt100 = 0,
    vt220 = 1,
    vt240 = 2,
    vt330 = 18,
    vt340 = 19,
    vt320 = 24,
    vt382 = 32,
    vt420 = 41,
    vt510 = 61,
    vt520 = 64,
    vt525 = 65,
};

/* Primary device attributes (DA1).
 *
 * Response format: CSI ? Pp ; Ps... c
 * where Pp is the conformance level and Ps are feature flags. */
struct Primary {
    /* DA1 feature attribute codes. */
    enum class Feature : uint16_t {
        columns_132 = 1,
        printer = 2,
        regis = 3,
        sixel = 4,
        selective_erase = 6,
        user_defined_keys = 8,
        national_replacement = 9,
        technical_characters = 15,
        locator = 16,
        terminal_state = 17,
        windowing = 18,
        horizontal_scrolling = 21,
        ansi_color = 22,
        rectangular_editing = 28,
        ansi_text_locator = 29,
        clipboard = 52,
    };

    /* Conformance level sent as the first parameter. */
    ConformanceLevel conformance_level;

    /* Optional feature attributes.
     * Wisp: []const Feature is a pointer and length; the default points at
     * a static { .ansi_color }. */
    const Feature *features;
    size_t features_len;

    Primary() : conformance_level(ConformanceLevel::vt220) {
        static const Feature default_features[] = { Feature::ansi_color };
        features = default_features;
        features_len = 1;
    }

    /* Encode the primary DA response into the writer. */
    void encode(std::string *writer) const {
        char buf[16];
        snprintf(buf, sizeof(buf), "\x1b[?%u", (unsigned)conformance_level);
        writer->append(buf);
        for (size_t i = 0; i < features_len; i++) {
            snprintf(buf, sizeof(buf), ";%u", (unsigned)features[i]);
            writer->append(buf);
        }
        writer->append("c");
    }
};

/* Secondary device attributes (DA2).
 *
 * Response format: CSI > Pp ; Pv ; Pc c */
struct Secondary {
    /* Terminal type identifier (Pp parameter from secondary DA response). */
    DeviceType device_type;

    /* Firmware/patch version number. */
    uint16_t firmware_version;

    /* ROM cartridge registration number. Always 0 for emulators. */
    uint16_t rom_cartridge;

    Secondary() : device_type(DeviceType::vt220), firmware_version(0), rom_cartridge(0) {}

    /* Encode the secondary DA response into the writer. */
    void encode(std::string *writer) const {
        char buf[48];
        snprintf(buf, sizeof(buf), "\x1b[>%u;%u;%uc",
                 (unsigned)device_type, (unsigned)firmware_version,
                 (unsigned)rom_cartridge);
        writer->append(buf);
    }
};

/* Tertiary device attributes (DA3).
 *
 * Response format: DCS ! | D...D ST
 * where D...D is the unit ID as hex digits (DECRPTUI). */
struct Tertiary {
    /* Unit ID (DECRPTUI). Encoded as 8 uppercase hex digits.
     * Meaningless for emulators nowadays. The actual DEC manuals
     * appear to split this into two 16-bit fields but since there
     * is no practical usage I know if I'm simplifying this. */
    uint32_t unit_id;

    Tertiary() : unit_id(0) {}

    /* Encode the tertiary DA response into the writer. */
    void encode(std::string *writer) const {
        char buf[32];
        snprintf(buf, sizeof(buf), "\x1bP!|%08X\x1b\\", (unsigned)unit_id);
        writer->append(buf);
    }
};

/* Response data for all device attribute queries. */
struct Attributes {
    /* Reply to CSI c (DA1). */
    Primary primary;

    /* Reply to CSI > c (DA2). */
    Secondary secondary;

    /* Reply to CSI = c (DA3). */
    Tertiary tertiary;

    /* Encode the response for the given request type into the writer. */
    void encode(Req req, std::string *writer) const {
        switch (req) {
            case Req::primary: primary.encode(writer); break;
            case Req::secondary: secondary.encode(writer); break;
            case Req::tertiary: tertiary.encode(writer); break;
        }
    }
};

} /* namespace device_attributes */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_DEVICE_ATTRIBUTES_HPP */
