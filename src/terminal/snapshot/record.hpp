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

#include <assert.h>
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

    /* std.Io.Writer.Error || error{PayloadTooLarge}.
     *
     * Wisp: WriteFailed is upstream's destination error. A `std::string *`
     * destination cannot fail, so it is never returned. */
    enum class FinishError : uint8_t {
        none,
        WriteFailed,

        /* The payload cannot be represented by the record's `u32` length. */
        PayloadTooLarge,
    };

    /* Finish and emit the active record's header followed by its payload.
     *
     * Payload validation failures occur before this function and emit no part
     * of the record. A destination failure here may have emitted a prefix. */
    inline FinishError finish() {
        assert(has_active);
        const Tag tag = active_tag;

        /* defer self.cancel() */
        struct CancelGuard {
            Writer *w;
            ~CancelGuard() { w->cancel(); }
        } guard = { this };

        const std::string &payload = scratch;
        if ((uint64_t)payload.size() > 0xFFFFFFFFull) return FinishError::PayloadTooLarge;
        const uint32_t payload_len = (uint32_t)payload.size();

        /* The CRC prefix contains the payload length, so checksum the retained
         * payload only after its final length is known. */
        Checksum checksum;
        checksum.init(tag, payload_len);
        checksum.update(payload);

        Header header;
        header.tag = tag;
        header.payload_len = payload_len;
        header.crc32c = checksum.final();

        header.encode(destination);
        destination->append(payload);
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

/* The length-limited, checksum-updating payload reader returned by
 * `Reader.payloadReader`.
 *
 * Wisp: upstream composes `std.Io.Reader.Limited` and
 * `std.Io.Reader.Hashed(Crc32c)` over the source. This is that composition
 * as one type: every byte read through it, discarded through it, or
 * attempted past its limit is drawn from the source, counted against
 * `payload_len`, and hashed. A read that asks for more than remains
 * consumes and hashes what remains and then reports EndOfStream, exactly
 * as the Limited adapter does, so the source is never advanced into the
 * following record.
 *
 * The source is type-erased behind a read thunk so one Reader type serves
 * every source reader shape, as upstream's interface pointer does. */
struct PayloadReader {
    void *source = nullptr;
    bool (*source_read)(void *source, void *dest, size_t n, zigstd::ReadError *err) = nullptr;
    size_t limit_remaining = 0;
    Checksum checksum;

    /* Wisp: backing storage for `take`, whose upstream slice points into the
     * reader's own buffer. Valid until the next `take`. */
    std::string take_buf;

    /* Wisp: one byte drawn by `peekByte` and not yet consumed. It has
     * already passed the limit and the hasher, so `finish` treats it as
     * buffered and therefore not exhausted, as upstream's hashing reader
     * does with its buffer. */
    bool has_peek = false;
    uint8_t peek_byte = 0;

    /* std.Io.Reader.readSliceAll through the limit and the hasher. */
    bool readSliceAll(void *dest, size_t n, zigstd::ReadError *err) {
        if (n > 0 && has_peek) {
            *(uint8_t *)dest = peek_byte;
            has_peek = false;
            return readSliceAll((uint8_t *)dest + 1, n - 1, err);
        }
        const size_t take_n = n < limit_remaining ? n : limit_remaining;
        if (take_n > 0) {
            if (!source_read(source, dest, take_n, err)) {
                limit_remaining = 0;
                if (err != nullptr) *err = zigstd::ReadError::EndOfStream;
                return false;
            }
            checksum.update((const uint8_t *)dest, take_n);
            limit_remaining -= take_n;
        }
        if (take_n < n) {
            if (err != nullptr) *err = zigstd::ReadError::EndOfStream;
            return false;
        }
        return true;
    }

    /* std.Io.Reader.takeByte */
    bool takeByte(uint8_t *out, zigstd::ReadError *err) {
        return readSliceAll(out, 1, err);
    }

    /* std.Io.Reader.take */
    const uint8_t *take(size_t n, zigstd::ReadError *err) {
        take_buf.resize(n);
        if (!readSliceAll(n ? &take_buf[0] : nullptr, n, err)) return nullptr;
        return (const uint8_t *)take_buf.data();
    }

    /* std.Io.Reader.discardAll; discarded bytes still pass the hasher. */
    bool discardAll(size_t n, zigstd::ReadError *err) {
        uint8_t scratch[256];
        while (n > 0) {
            const size_t step = n < sizeof scratch ? n : sizeof scratch;
            if (!readSliceAll(scratch, step, err)) return false;
            n -= step;
        }
        return true;
    }

    /* std.Io.Reader.peekByte */
    bool peekByte(uint8_t *out, zigstd::ReadError *err) {
        if (!has_peek) {
            uint8_t b = 0;
            if (!readSliceAll(&b, 1, err)) return false;
            peek_byte = b;
            has_peek = true;
        }
        *out = peek_byte;
        return true;
    }

    /* std.Io.Reader.discardRemaining: consume (and hash) the rest of the
     * payload. Returns the number of bytes discarded. */
    size_t discardRemaining() {
        size_t n = has_peek ? 1 : 0;
        has_peek = false;
        const size_t rest = limit_remaining;
        zigstd::ReadError ignored = zigstd::ReadError::none;
        if (discardAll(rest, &ignored)) n += rest;
        return n;
    }

    /* Payload bytes not yet consumed. */
    size_t remaining() const { return limit_remaining + (has_peek ? 1 : 0); }
};

/* Reads one complete record.
 *
 * `init` decodes the header, `payloadReader` returns a reader limited to the
 * declared payload, and `finish` verifies exact exhaustion and the CRC32C.
 *
 * Wisp: upstream additionally borrows the payload in place when the source
 * already has it fully buffered, verifying the CRC in one pass in `finish`.
 * That is a performance path over the same contract; the port's readers do
 * not expose their buffer, so every record streams. */
struct Reader {
    Header header;
    PayloadReader payload;

    enum class InitError : uint8_t {
        none,
        EndOfStream,
        InvalidTag,
    };

    /* Errors detected after a payload decoder returns. */
    enum class FinishError : uint8_t {
        none,

        /* The payload bytes do not match the CRC in the record header. */
        InvalidChecksum,

        /* The decoder did not consume exactly `payload_len` bytes. */
        PayloadNotExhausted,
    };

    template <typename SourceT>
    static bool readThunk(void *source, void *dest, size_t n, zigstd::ReadError *err) {
        return ((SourceT *)source)->readSliceAll(dest, n, err);
    }

    /* Decode a record header and initialize its payload reader.
     *
     * `source` must outlive this reader. Decode the payload only through the
     * reader returned by `payloadReader`. */
    template <typename SourceT>
    bool init(SourceT *source, InitError *err) {
        DecodeError decode_err = DecodeError::none;
        if (!Header::decode(source, &header, &decode_err)) {
            *err = decode_err == DecodeError::InvalidTag ? InitError::InvalidTag
                                                         : InitError::EndOfStream;
            return false;
        }

        payload.source = (void *)source;
        payload.source_read = &readThunk<SourceT>;
        payload.limit_remaining = header.payload_len;
        payload.has_peek = false;
        payload.checksum.init(header.tag, header.payload_len);
        *err = InitError::none;
        return true;
    }

    /* Return the length-limited, checksum-updating payload reader. */
    PayloadReader *payloadReader() { return &payload; }

    /* Require exact payload exhaustion and validate its CRC32C. */
    FinishError finish() {
        if (payload.has_peek || payload.limit_remaining != 0) return FinishError::PayloadNotExhausted;
        if (payload.checksum.final() != header.crc32c) return FinishError::InvalidChecksum;
        return FinishError::none;
    }
};

} /* namespace record */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_RECORD_HPP */
