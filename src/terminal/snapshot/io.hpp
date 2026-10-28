/* Transliterated from Ghostty src/terminal/snapshot/io.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wire-format I/O helpers.
 *
 * These are sometimes very thin layers over standard `std.Io.Writer`
 * but its to help ensure consistency in our behavior and avoid some
 * pitfalls.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `comptime T: type` becomes a template parameter. Only the unsigned
 *     widths the snapshot format uses are instantiated.
 *   - `*std.Io.Writer` becomes `std::string *`, as everywhere in the
 *     port. A string append cannot fail, so writeInt returns void where
 *     upstream returns `std.Io.Writer.Error!void`.
 *   - `*std.Io.Reader` becomes zigstd::Reader or zigstd::LimitedReader
 *     (see zigstd/reader.hpp). readInt is a template over the reader
 *     type so both work, which is what upstream gets for free by taking
 *     the interface pointer.
 *   - `std.Io.Reader.Error!T` becomes a bool return with the value in an
 *     out parameter and the error in another.
 *   - Little-endian is assembled byte by byte rather than by
 *     std.mem.readInt, so the result does not depend on the host's
 *     endianness. Every target of this port is little-endian, but the
 *     wire format says little-endian and the code should say it too.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_IO_HPP
#define WISP_TERMINAL_SNAPSHOT_IO_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "../../zigstd/reader.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace io {

typedef ::wisp::zigstd::ReadError ReadError;

/* writeInt ensures we write in little-endian since that is our
 * standard form. */
template <typename T>
inline void writeInt(std::string *writer, T value) {
    for (size_t i = 0; i < sizeof(T); i++) {
        writer->push_back((char)(uint8_t)((value >> (8 * i)) & 0xFF));
    }
}

/* Read a little-endian integer.
 *
 * This avoids `takeInt` because at the time of writing this the underlying
 * implementation uses `takeArray` which asserts that the reader buffer
 * is as large as T. Our approach works with caller-selected buffers that
 * may be smaller. */
template <typename T, typename ReaderT>
inline bool readInt(ReaderT *reader, T *out, ReadError *err) {
    uint8_t buf[sizeof(T)];
    if (!reader->readSliceAll(buf, sizeof(T), err)) return false;

    T value = 0;
    for (size_t i = 0; i < sizeof(T); i++) {
        value = (T)(value | ((T)buf[i] << (8 * i)));
    }
    *out = value;
    return true;
}

} /* namespace io */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_IO_HPP */
