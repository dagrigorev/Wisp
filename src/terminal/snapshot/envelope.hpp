/* Transliterated from Ghostty src/terminal/snapshot/envelope.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Snapshot envelope.
 *
 * Every snapshot binary blob begins with exactly one envelope at byte zero.
 * It is followed by a set of records. Each record provides their own
 * tag, payload length, CRC, etc.
 *
 * The envelope identifies the bytes as a terminal snapshot and selects the
 * single version governing the entire blob. A decoder validates both fields
 * before reading any records.
 *
 * The envelope is exactly ten bytes. All integers are unsigned and
 * little-endian.
 *
 * | Offset | Size | Field                |
 * | -----: | ---: | :------------------- |
 * |      0 |    8 | Magic (`GHOSTSNP`)   |
 * |      8 |    2 | Version (`u16`)      |
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` becomes `std::string *` and `*std.Io.Reader`
 *     becomes a zigstd reader; decode is a template so both reader
 *     shapes work, as upstream's interface pointer allows.
 *   - `DecodeError` becomes an `Error` enum carrying upstream's three
 *     cases: EndOfStream (from the reader), InvalidMagic and
 *     UnsupportedVersion. `encode` cannot fail, a string append having
 *     nowhere to fail to, so it returns void.
 *   - `encoded_len` is computed by upstream's `computeLen()`, which
 *     encodes into a comptime buffer and reads the writer's end offset.
 *     Here it is a constant with the same comptime assertion that it is
 *     10, which is upstream's own guard against the layout drifting --
 *     see the assert below. Deriving it by running encode() at compile
 *     time would need encode to be constexpr for no benefit; the assert
 *     is what actually catches a change.
 *   - `magic` is a `[8]u8` view rather than a Zig string literal, so
 *     its length is explicit and no NUL is implied. Upstream relies on
 *     `magic.len` being 8 for exactly the same reason.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_ENVELOPE_HPP
#define WISP_TERMINAL_SNAPSHOT_ENVELOPE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../zigstd/reader.hpp"
#include "io.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace envelope {

/* Identifies a Ghostty terminal snapshot and rejects unrelated input before
 * any record decoding begins. All eight bytes are part of the wire value. */
static const char magic[] = {'G', 'H', 'O', 'S', 'T', 'S', 'N', 'P'};
static const size_t magic_len = sizeof(magic);

/* The complete compatibility boundary for snapshot layout and behavior.
 * Version 1 readers require this value to match exactly. */
static const uint16_t version = 1;

/* Number of bytes in the fixed envelope: magic followed by version. */
static const size_t encoded_len = magic_len + sizeof(uint16_t);

/* We expect this so if it changes we should think carefully. */
static_assert(encoded_len == 10, "snapshot envelope must be ten bytes");

/* Wisp: std.Io.Reader.Error || error{InvalidMagic, UnsupportedVersion}. */
enum class Error : uint8_t {
    none,
    EndOfStream,
    InvalidMagic,
    UnsupportedVersion,
};

/* Encode the envelope. */
inline void encode(std::string *writer) {
    writer->append(magic, magic_len);
    io::writeInt<uint16_t>(writer, version);
}

/* Decode and validate the envelope. */
template <typename ReaderT>
inline bool decode(ReaderT *reader, Error *err) {
    char actual_magic[magic_len];
    ::wisp::zigstd::ReadError rerr = ::wisp::zigstd::ReadError::none;
    if (!reader->readSliceAll(actual_magic, magic_len, &rerr)) {
        if (err != nullptr) *err = Error::EndOfStream;
        return false;
    }
    if (memcmp(magic, actual_magic, magic_len) != 0) {
        if (err != nullptr) *err = Error::InvalidMagic;
        return false;
    }

    uint16_t actual_version = 0;
    if (!io::readInt<uint16_t>(reader, &actual_version, &rerr)) {
        if (err != nullptr) *err = Error::EndOfStream;
        return false;
    }
    if (actual_version != version) {
        if (err != nullptr) *err = Error::UnsupportedVersion;
        return false;
    }
    return true;
}

} /* namespace envelope */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_ENVELOPE_HPP */
