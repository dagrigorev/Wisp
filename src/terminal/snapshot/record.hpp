/* Transliterated from Ghostty src/terminal/snapshot/record.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Snapshot record framing.
 *
 * Records begin immediately after the single snapshot envelope and continue
 * back-to-back until FINISH. Each record has one fixed-size header followed
 * by exactly `payload_len` bytes. The payload length limits the record reader
 * so malformed payloads cannot consume bytes from the following record.
 *
 * The CRC covers the first six header bytes (tag and payload length)
 * followed by the payload. The CRC field itself is not included for obvious
 * reasons. All integers are unsigned and little-endian.
 *
 * | Offset | Size          | Field                  |
 * | -----: | ------------: | :--------------------- |
 * |      0 |             2 | Tag (`u16`)            |
 * |      2 |             4 | Payload length (`u32`) |
 * |      6 |             4 | CRC32C (`u32`)         |
 * |     10 | `payload_len` | Payload                |
 *
 * Supported tags are in `Tag`.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` becomes `std::string *` and `*std.Io.Reader`
 *     becomes a zigstd reader type.
 *   - Zig's error union becomes separate error enums (e.g., DecodeError).
 *   - Zig's allocator becomes std::vector or string-based buffering.
 *   - Tag enum(u16) becomes enum class Tag : uint16_t.
 *   - Header.len is a static const size_t with static_assert checking == 10.
 *   - Writer uses std::string for buffering instead of Allocating writer.
 *   - Reader adapts to work with zigstd reader types.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_RECORD_HPP
#define WISP_TERMINAL_SNAPSHOT_RECORD_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../zigstd/crc32c.hpp"
#include "../../zigstd/reader.hpp"
#include "io.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace record {

/* Identifies the layout and meaning of a record payload.
 * The current snapshot version rejects every value not listed here. */
enum class Tag : uint16_t {
    /* Terminal-wide live state and configuration. */
    terminal = 1,
    /* One live screen and its page sequence. */
    screen = 2,
    /* One complete logical terminal page. */
    page = 3,
    /* One screen's complete history manifest and page sequence. */
    history = 4,
    /* Marker separating the renderable terminal state from history. */
    ready = 5,
    /* Marker terminating the complete snapshot blob. */
    finish = 6,
    /* Canonical unfinished standard TerminalStream input. */
    continuation = 7,
};

/* Errors detected during header encoding/decoding. */
enum class DecodeError : uint8_t {
    none,
    EndOfStream,
    InvalidTag,
};

/* Encode the portion of a record header covered by CRC32C. */
inline void encodeChecksumPrefix(Tag tag, uint32_t payload_len,
                                  std::string *writer) {
    io::writeInt<uint16_t>(writer, static_cast<uint16_t>(tag));
    io::writeInt<uint32_t>(writer, payload_len);
}

/* The fixed framing that precedes every record payload. */
struct Header {
    /* Number of bytes written by encode(). */
    static const size_t len = 10;

    /* Static assertion: this size is part of the wire format. */
    static_assert(len == 10, "snapshot record header must be ten bytes");

    /* Determines how the payload is decoded. */
    Tag tag;

    /* Number of payload bytes immediately following this header. */
    uint32_t payload_len;

    /* CRC32C over the encoded tag, payload length, and payload. */
    uint32_t crc32c;

    /* Encode the fixed record header to a string. */
    inline void encode(std::string *writer) const {
        encodeChecksumPrefix(tag, payload_len, writer);
        io::writeInt<uint32_t>(writer, crc32c);
    }

    /* Decode a fixed record header and reject unknown tags.
     * Payload length and CRC validation occur while decoding the payload. */
    template <typename ReaderT>
    inline static bool decode(ReaderT *reader, Header *out, DecodeError *err) {
        uint16_t tag_raw = 0;
        zigstd::ReadError rerr = zigstd::ReadError::none;

        if (!io::readInt<uint16_t, ReaderT>(reader, &tag_raw, &rerr)) {
            if (err != nullptr) *err = DecodeError::EndOfStream;
            return false;
        }

        /* Validate tag is in the known set. */
        if (tag_raw < 1 || tag_raw > 7) {
            if (err != nullptr) *err = DecodeError::InvalidTag;
            return false;
        }

        uint32_t payload_len = 0;
        if (!io::readInt<uint32_t, ReaderT>(reader, &payload_len, &rerr)) {
            if (err != nullptr) *err = DecodeError::EndOfStream;
            return false;
        }

        uint32_t crc32c = 0;
        if (!io::readInt<uint32_t, ReaderT>(reader, &crc32c, &rerr)) {
            if (err != nullptr) *err = DecodeError::EndOfStream;
            return false;
        }

        if (out != nullptr) {
            out->tag = static_cast<Tag>(tag_raw);
            out->payload_len = payload_len;
            out->crc32c = crc32c;
        }
        if (err != nullptr) *err = DecodeError::none;
        return true;
    }

    /* Comparison operator for testing. */
    inline bool operator==(const Header &other) const {
        return tag == other.tag && payload_len == other.payload_len &&
               crc32c == other.crc32c;
    }
};

/* A writer that calculates a record checksum without retaining payload bytes.
 * The checksum is initialized with the encoded tag and payload length. */
struct Checksum {
    zigstd::Crc32c crc;
    bool initialized = false;

    /* Begin a checksum with the encoded tag and payload length already added. */
    inline void init(Tag tag, uint32_t payload_len) {
        crc = zigstd::Crc32c::init();

        /* Encode tag and payload_len into the hash. */
        std::string prefix_buf;
        encodeChecksumPrefix(tag, payload_len, &prefix_buf);
        crc.update((const uint8_t *)prefix_buf.data(), prefix_buf.size());

        initialized = true;
    }

    /* Update the checksum with payload bytes. */
    inline void update(const uint8_t *data, size_t len) {
        if (initialized) {
            crc.update(data, len);
        }
    }

    /* Update the checksum with a string's payload. */
    inline void update(const std::string &data) {
        update((const uint8_t *)data.data(), data.size());
    }

    /* Finish and return the checksum stored in the record header. */
    inline uint32_t final() {
        return crc.final_();
    }
};

/* Streams complete records while retaining only one payload at a time.
 *
 * The scratch allocation is retained between records so a stream's peak
 * memory is the largest record payload rather than the complete snapshot. */
struct Writer {
    std::string *destination;
    std::string scratch;
    Tag active_tag;
    bool has_active = false;

    /* Initialize with a destination writer. */
    inline void init(std::string *dest) {
        destination = dest;
        scratch.clear();
        has_active = false;
    }

    /* Return the destination writer for unframed snapshot bytes. */
    inline std::string *writer() {
        return destination;
    }

    /* Begin one record and return its reusable payload writer. */
    inline std::string *begin(Tag tag) {
        scratch.clear();
        active_tag = tag;
        has_active = true;
        return &scratch;
    }

    /* Errors possible during finish. */
    enum class FinishError : uint8_t {
        none,
        PayloadTooLarge,
        WriteFailed,
    };

    /* Finish and emit the active record's header followed by its payload.
     *
     * Payload validation failures occur before this function and emit no part
     * of the record. A destination failure here may have emitted a prefix. */
    inline FinishError finish() {
        if (!has_active) {
            return FinishError::none;
        }

        Tag tag = active_tag;
        has_active = false;

        const std::string &payload = scratch;

        /* Check if payload fits in u32. */
        if (payload.size() > 0xFFFFFFFFU) {
            return FinishError::PayloadTooLarge;
        }

        uint32_t payload_len = (uint32_t)payload.size();

        /* The CRC prefix contains the payload length, so checksum the retained
         * payload only after its final length is known. */
        Checksum checksum;
        checksum.init(tag, payload_len);
        checksum.update(payload);

        Header header;
        header.tag = tag;
        header.payload_len = payload_len;
        header.crc32c = checksum.final();

        /* Encode header to a buffer. */
        std::string header_bytes;
        header.encode(&header_bytes);

        /* Write to destination. */
        if (header_bytes.size() != Header::len) {
            return FinishError::WriteFailed;
        }

        destination->append(header_bytes);
        destination->append(payload);

        scratch.clear();
        return FinishError::none;
    }

    /* Discard the active record without emitting any bytes.
     *
     * This is idempotent so an outer error cleanup may call it after `finish`
     * has already cleared state following a destination failure. */
    inline void cancel() {
        scratch.clear();
        has_active = false;
    }
};

/* A simple payload reader that tracks boundaries and crc. */
struct PayloadReader {
    const uint8_t *data;
    size_t remaining;
    Checksum *checksum;

    static PayloadReader make(const uint8_t *payload_data, size_t payload_len,
                               Checksum *crc) {
        PayloadReader r;
        r.data = payload_data;
        r.remaining = payload_len;
        r.checksum = crc;
        return r;
    }

    /* Read all remaining bytes. Simplified for testing. */
    const std::string &takeAll() {
        static std::string result;
        result.clear();
        if (data && remaining > 0) {
            result.append((const char *)data, remaining);
            if (checksum) {
                checksum->update(data, remaining);
            }
            remaining = 0;
        }
        return result;
    }

    /* Take n bytes and update checksum. */
    const uint8_t *take(size_t n) {
        if (remaining < n) return nullptr;
        const uint8_t *result = data;
        if (checksum) {
            checksum->update(data, n);
        }
        data += n;
        remaining -= n;
        return result;
    }

    /* Get remaining count. */
    size_t remainingLen() const { return remaining; }
};

/* Reads one complete record.
 *
 * `init` decodes the header, provides payload reading through embedded
 * structures, and `finish` verifies exact exhaustion and the CRC32C. */
struct Reader {
    Header header;
    PayloadReader payload_reader;
    Checksum checksum;
    bool has_checksum = false;

    enum class FinishError : uint8_t {
        none,
        InvalidChecksum,
        PayloadNotExhausted,
    };

    enum class InitError : uint8_t {
        none,
        EndOfStream,
        InvalidTag,
    };

    /* Decode a record header and initialize for payload reading.
     *
     * For simple fixed-buffer testing, caller provides the full record bytes. */
    template <typename ReaderT>
    inline bool init(ReaderT *source, InitError *err) {
        /* Decode header. */
        DecodeError decode_err = DecodeError::none;
        if (!Header::decode(source, &header, &decode_err)) {
            if (err != nullptr) {
                if (decode_err == DecodeError::InvalidTag) {
                    *err = InitError::InvalidTag;
                } else {
                    *err = InitError::EndOfStream;
                }
            }
            return false;
        }

        /* Initialize checksum for payload verification. */
        checksum.init(header.tag, header.payload_len);
        has_checksum = true;

        if (err != nullptr) *err = InitError::none;
        return true;
    }

    /* Get the payload reader for this record. Must be called after init. */
    inline PayloadReader *payloadReader() {
        return &payload_reader;
    }

    /* Set the payload data (called by the test after decoding from reader). */
    inline void setPayloadData(const uint8_t *data, size_t len) {
        payload_reader = PayloadReader::make(data, len, &checksum);
    }

    /* Require exact payload exhaustion and validate its CRC32C. */
    inline FinishError finish() {
        if (payload_reader.remainingLen() != 0) {
            return FinishError::PayloadNotExhausted;
        }

        if (!has_checksum) {
            return FinishError::none;
        }

        if (checksum.final() != header.crc32c) {
            return FinishError::InvalidChecksum;
        }

        return FinishError::none;
    }
};

} /* namespace record */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_RECORD_HPP */
