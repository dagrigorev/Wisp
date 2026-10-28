/* Transliterated from Ghostty src/terminal/snapshot/checkpoint.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * READY and FINISH snapshot marker records.
 *
 * READY separates the renderable terminal state from the history sequences.
 * FINISH terminates one snapshot; bytes after it belong to the containing
 * transport. Both markers have empty payloads, independently framed and
 * protected by the same CRC32C as every other record.
 *
 * ## Binary Format
 *
 * READY and FINISH contain only their record headers. Their declared payload
 * length must be zero.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Zig's enum becomes enum class
 *   - Zig's error union becomes DecodeError enum
 *   - std.Io.Reader becomes a template-based reader type
 *   - Tests use catch2
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_CHECKPOINT_HPP
#define WISP_TERMINAL_SNAPSHOT_CHECKPOINT_HPP

#include <stdint.h>

#include "record.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace checkpoint {

/* Selects one of the two marker positions in a snapshot. */
enum class Kind : uint8_t {
    ready = 0,
    finish = 1,
};

/* Convert Kind to the corresponding record Tag. */
inline record::Tag kind_to_tag(Kind kind) {
    switch (kind) {
        case Kind::ready:
            return record::Tag::ready;
        case Kind::finish:
            return record::Tag::finish;
    }
    // Unreachable, but required for some compilers
    return record::Tag::ready;
}

/* Errors possible while encoding a marker. */
using EncodeError = record::Writer::FinishError;

/* Append one empty marker record. */
inline EncodeError encode(Kind kind, record::Writer *stream) {
    stream->begin(kind_to_tag(kind));
    return stream->finish();
}

/* Errors possible while decoding a marker. */
enum class DecodeError : uint8_t {
    none,
    EndOfStream,
    InvalidTag,
    /* The next record is valid but is not the expected marker. */
    UnexpectedRecordTag,
    /* The record's payload is not empty. */
    PayloadNotExhausted,
};

/* Decode one marker and require its payload to be empty. */
template <typename ReaderT>
inline DecodeError decode(Kind kind, ReaderT *source) {
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;

    if (!record_reader.init(source, &init_err)) {
        if (init_err == record::Reader::InitError::InvalidTag) {
            return DecodeError::InvalidTag;
        } else {
            return DecodeError::EndOfStream;
        }
    }

    if (record_reader.header.tag != kind_to_tag(kind)) {
        return DecodeError::UnexpectedRecordTag;
    }

    if (record_reader.header.payload_len != 0) {
        return DecodeError::PayloadNotExhausted;
    }

    /* For empty payloads, set payload data to empty and verify finish.
     * This will validate the CRC and ensure no bytes were consumed. */
    record_reader.setPayloadData(nullptr, 0);

    record::Reader::FinishError finish_err = record_reader.finish();
    if (finish_err != record::Reader::FinishError::none) {
        return DecodeError::PayloadNotExhausted;
    }

    return DecodeError::none;
}

} /* namespace checkpoint */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_CHECKPOINT_HPP */
