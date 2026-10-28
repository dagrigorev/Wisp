/* Transliterated from Ghostty src/terminal/snapshot/continuation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Continuation record type.
 *
 * Continuation uses the standard record header. All trailing bytes
 * are payload bytes for the continuation state.
 *
 * A continuation's payload must be the minimal bytes to go from
 * a ground state to a non-ground state in a VT emulator. Any extra
 * bytes are treated as a validation error.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Zig's union(enum) becomes tagged variant (tag + union-like storage).
 *   - Zig's error union becomes separate error enums.
 *   - Zig's Allocator becomes std::vector-based or string-based.
 *   - Reader type matches the other snapshot modules.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_CONTINUATION_HPP
#define WISP_TERMINAL_SNAPSHOT_CONTINUATION_HPP

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <vector>
#include <string>

#include "record.hpp"
#include "../stream_continuation.hpp"
#include "../../zigstd/reader.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace continuation {

/* A borrowed continuation value supplied to encode, or an allocator-owned
 * value stored in a decoded complete snapshot.
 *
 * Wisp: Zig union(enum) becomes a tagged struct with optional payload. */
struct Value {
    enum class Tag : uint8_t {
        ground,  /* Neither the VT parser nor UTF-8 decoder has unfinished state. */
        bytes,   /* Canonical replay-safe PTY bytes. Empty is equivalent to `ground`. */
    };

    Tag tag;
    std::vector<uint8_t> bytes_payload;  /* Only used when tag == bytes. */

    /* Default to ground. */
    Value() : tag(Tag::ground), bytes_payload() {}

    /* Create a ground value. */
    static Value makeGround() {
        Value v;
        v.tag = Tag::ground;
        return v;
    }

    /* Create a bytes value from a slice. */
    static Value makeBytes(const uint8_t *data, size_t len) {
        Value v;
        v.tag = Tag::bytes;
        v.bytes_payload.assign(data, data + len);
        return v;
    }

    /* Create a bytes value from a string. */
    static Value makeBytes(const std::string &str) {
        Value v;
        v.tag = Tag::bytes;
        v.bytes_payload.assign(str.begin(), str.end());
        return v;
    }

    /* Comparison for testing. */
    inline bool operator==(const Value &other) const {
        if (tag != other.tag) return false;
        if (tag == Tag::ground) return true;
        return bytes_payload == other.bytes_payload;
    }
};

/* Errors possible while validating a continuation. */
enum class ValidateError : uint8_t {
    none,
    Overflow,  /* The common record's u32 payload length cannot represent these bytes. */
    NoPendingState,
    NonCanonicalContinuation,
    ReplayWouldCommit,
};

/* Validate a continuation before any snapshot bytes are emitted. */
inline ValidateError validate(const Value &value) {
    switch (value.tag) {
        case Value::Tag::ground:
            return ValidateError::none;
        case Value::Tag::bytes: {
            if (value.bytes_payload.size() > 0xFFFFFFFFU) {
                return ValidateError::Overflow;
            }
            stream_continuation::ValidateError err =
                stream_continuation::validate(value.bytes_payload.data(),
                                              value.bytes_payload.size());
            if (err == stream_continuation::ValidateError::none) {
                return ValidateError::none;
            } else if (err == stream_continuation::ValidateError::NoPendingState) {
                return ValidateError::NoPendingState;
            } else if (err == stream_continuation::ValidateError::NonCanonicalContinuation) {
                return ValidateError::NonCanonicalContinuation;
            } else if (err == stream_continuation::ValidateError::ReplayWouldCommit) {
                return ValidateError::ReplayWouldCommit;
            }
            return ValidateError::none;
        }
    }
    return ValidateError::none;
}

/* Errors possible during encoding. */
enum class EncodeError : uint8_t {
    none,
    Overflow,
    NoPendingState,
    NonCanonicalContinuation,
    ReplayWouldCommit,
    PayloadTooLarge,
    WriteFailed,
};

/* Encode one complete CONTINUATION record. */
inline EncodeError encode(const Value &value, record::Writer *stream) {
    ValidateError verr = validate(value);
    if (verr != ValidateError::none) {
        return static_cast<EncodeError>(verr);
    }

    std::string *payload = stream->begin(record::Tag::continuation);
    if (payload == nullptr) {
        return EncodeError::WriteFailed;
    }

    switch (value.tag) {
        case Value::Tag::ground:
            /* Empty payload for ground state. */
            break;
        case Value::Tag::bytes:
            payload->append((const char *)value.bytes_payload.data(), value.bytes_payload.size());
            break;
    }

    record::Writer::FinishError ferr = stream->finish();
    if (ferr != record::Writer::FinishError::none) {
        if (ferr == record::Writer::FinishError::PayloadTooLarge) {
            return EncodeError::PayloadTooLarge;
        }
        return EncodeError::WriteFailed;
    }

    return EncodeError::none;
}

/* Errors possible during decoding. */
enum class DecodeError : uint8_t {
    none,
    Allocator,
    NoPendingState,
    NonCanonicalContinuation,
    ReplayWouldCommit,
    EndOfStream,
    InvalidTag,
    ContinuationLimitExceeded,
    InvalidChecksum,
    PayloadNotExhausted,
    /* The next record is valid but is not a CONTINUATION. */
    UnexpectedRecordTag,
};

inline DecodeError mapFinish(record::Reader::FinishError e) {
    switch (e) {
        case record::Reader::FinishError::none: return DecodeError::none;
        case record::Reader::FinishError::InvalidChecksum: return DecodeError::InvalidChecksum;
        case record::Reader::FinishError::PayloadNotExhausted: return DecodeError::PayloadNotExhausted;
    }
    return DecodeError::none;
}

/* Decode and validate one CONTINUATION record.
 *
 * A zero-length payload returns `ground` without allocating. A nonempty
 * payload returns allocator-owned bytes; although `Value.bytes_payload` is
 * internally owned, the caller gets a Value they can use directly.
 *
 * Wisp: We use std::vector internally; the return value owns the data. */
template <typename ReaderT>
inline DecodeError decode(ReaderT *source, Value *out, size_t max_bytes) {
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    if (!record_reader.init(source, &init_err)) {
        return init_err == record::Reader::InitError::InvalidTag ? DecodeError::InvalidTag
                                                                 : DecodeError::EndOfStream;
    }
    if (record_reader.header.tag != record::Tag::continuation) {
        return DecodeError::UnexpectedRecordTag;
    }

    /* Validate the header payload vs our max size since the header
     * enforces it. */
    const size_t len = record_reader.header.payload_len;
    if (len > max_bytes) return DecodeError::ContinuationLimitExceeded;

    if (len == 0) {
        /* Empty is the ground representation, but finish is still required:
         * its CRC covers the tag and zero length even though there is no body. */
        const DecodeError ferr = mapFinish(record_reader.finish());
        if (ferr != DecodeError::none) return ferr;
        *out = Value::makeGround();
        return DecodeError::none;
    }

    /* Consume the bytes.
     * Wisp: the owned buffer is a std::vector, freed on every early return. */
    std::vector<uint8_t> bytes(len);
    zigstd::ReadError rerr = zigstd::ReadError::none;
    if (!record_reader.payloadReader()->readSliceAll(bytes.data(), len, &rerr)) {
        return DecodeError::EndOfStream;
    }
    const DecodeError ferr = mapFinish(record_reader.finish());
    if (ferr != DecodeError::none) return ferr;

    /* Framing is valid, so it is now safe to classify the bytes as replay
     * state. Validation requires a minimal, unfinished, side-effect-free tail. */
    switch (stream_continuation::validate(bytes.data(), bytes.size())) {
        case stream_continuation::ValidateError::none: break;
        case stream_continuation::ValidateError::NoPendingState: return DecodeError::NoPendingState;
        case stream_continuation::ValidateError::NonCanonicalContinuation:
            return DecodeError::NonCanonicalContinuation;
        case stream_continuation::ValidateError::ReplayWouldCommit: return DecodeError::ReplayWouldCommit;
    }

    /* Ownership transfers to the returned Value only after all validation. */
    *out = Value::makeBytes(bytes.data(), bytes.size());
    return DecodeError::none;
}

} /* namespace continuation */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_CONTINUATION_HPP */
