/* Wisp: original, not a transliteration of any single upstream file.
 *
 * A stand-in for `std.Io.Reader`, the counterpart to the `std::string *`
 * this port uses for `*std.Io.Writer`. The snapshot subsystem decodes a
 * wire format and every `decode` in it takes a reader, so one is needed
 * before any of those files can be ported.
 *
 * Two shapes, mirroring the two the snapshot code actually constructs:
 *
 *   Reader        - `std.Io.Reader.fixed(bytes)`: the whole input is in
 *                   memory and reads are a cursor walk.
 *   LimitedReader - `source.limited(.unlimited, &buf)`: reads draw
 *                   through a CALLER-PROVIDED buffer, refilling it as
 *                   needed.
 *
 * LimitedReader earns its place rather than being ceremony. Upstream's
 * snapshot/io.zig exists because `std.Io.Reader.takeInt` asserts the
 * reader's buffer is at least `@sizeOf(T)`, so its `readInt` reads into a
 * local array instead, and its one test proves that by pulling a u32
 * through a one-byte buffer. Model a reader as a bare cursor and that
 * test asserts nothing at all -- the hazard it guards against cannot
 * occur. With a real refilling buffer the test exercises the same thing
 * upstream's does: several refills to satisfy one read.
 *
 * Errors are a bool return plus an Error out-param, as elsewhere in the
 * port. EndOfStream is the only error either shape can produce; a short
 * read is always EndOfStream, never a partial success, because
 * readSliceAll is all-or-nothing as upstream's is.
 */
#pragma once
#ifndef WISP_ZIGSTD_READER_HPP
#define WISP_ZIGSTD_READER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace zigstd {

/* std.Io.Reader.Error. The readers in this file only ever produce
 * EndOfStream; ReadFailed exists for reader types that model a failing
 * source, as upstream's tests construct. */
enum class ReadError : uint8_t {
    none,
    EndOfStream,
    ReadFailed,
};

/* std.Io.Reader.fixed: a cursor over bytes already in memory. */
struct Reader {
    const uint8_t *buf;
    size_t len;
    size_t pos;

    Reader() : buf(nullptr), len(0), pos(0) {}

    static Reader fixed(const void *bytes, size_t n) {
        Reader r;
        r.buf = (const uint8_t *)bytes;
        r.len = n;
        r.pos = 0;
        return r;
    }

    size_t remaining() const { return len - pos; }

    /* std.Io.Reader.readSliceAll: fill dest entirely or fail, consuming
     * nothing on failure so a caller that retries sees the same stream. */
    bool readSliceAll(void *dest, size_t n, ReadError *err) {
        if (remaining() < n) {
            if (err != nullptr) *err = ReadError::EndOfStream;
            return false;
        }
        memcpy(dest, buf + pos, n);
        pos += n;
        return true;
    }

    /* std.Io.Reader.takeByte. */
    bool takeByte(uint8_t *out, ReadError *err) {
        return readSliceAll(out, 1, err);
    }

    /* std.Io.Reader.discardAll: skip exactly n bytes or fail. */
    bool discardAll(size_t n, ReadError *err) {
        if (remaining() < n) {
            pos = len;
            if (err != nullptr) *err = ReadError::EndOfStream;
            return false;
        }
        pos += n;
        return true;
    }

    /* std.Io.Reader.peekByte */
    bool peekByte(uint8_t *out, ReadError *err) {
        if (remaining() < 1) {
            if (err != nullptr) *err = ReadError::EndOfStream;
            return false;
        }
        *out = buf[pos];
        return true;
    }

    /* std.Io.Reader.discardRemaining */
    size_t discardRemaining() {
        const size_t n = remaining();
        pos = len;
        return n;
    }

    /* Hand out the next n bytes without copying. Not an upstream method;
     * used where upstream takes a slice of an already-fixed buffer. */
    const uint8_t *take(size_t n, ReadError *err) {
        if (remaining() < n) {
            if (err != nullptr) *err = ReadError::EndOfStream;
            return nullptr;
        }
        const uint8_t *p = buf + pos;
        pos += n;
        return p;
    }
};

/* `source.limited(.unlimited, &buf)`: reads are served from a
 * caller-provided buffer, refilled from the source as it empties. The
 * buffer may be smaller than an individual read, which is the whole
 * point -- see the file header. */
struct LimitedReader {
    Reader *source;
    uint8_t *buf;
    size_t buf_len;
    size_t buf_pos;   /* next unread byte in buf */
    size_t buf_fill;  /* bytes currently valid in buf */

    LimitedReader()
        : source(nullptr), buf(nullptr), buf_len(0), buf_pos(0), buf_fill(0) {}

    static LimitedReader limited(Reader *src, void *buffer, size_t buffer_len) {
        LimitedReader r;
        r.source = src;
        r.buf = (uint8_t *)buffer;
        r.buf_len = buffer_len;
        r.buf_pos = 0;
        r.buf_fill = 0;
        return r;
    }

    /* Satisfy the read across as many refills as it takes. A read larger
     * than the buffer is served in buffer-sized pieces. */
    bool readSliceAll(void *dest, size_t n, ReadError *err) {
        uint8_t *out = (uint8_t *)dest;
        size_t done = 0;

        while (done < n) {
            if (buf_pos == buf_fill) {
                /* Refill. A refill that comes back empty is end of
                 * stream: the read cannot be completed. */
                const size_t want = source->remaining() < buf_len
                                        ? source->remaining()
                                        : buf_len;
                if (want == 0) {
                    if (err != nullptr) *err = ReadError::EndOfStream;
                    return false;
                }
                if (!source->readSliceAll(buf, want, err)) return false;
                buf_pos = 0;
                buf_fill = want;
            }

            const size_t avail = buf_fill - buf_pos;
            const size_t need = n - done;
            const size_t take_n = avail < need ? avail : need;
            memcpy(out + done, buf + buf_pos, take_n);
            buf_pos += take_n;
            done += take_n;
        }
        return true;
    }

    /* std.Io.Reader.takeByte. */
    bool takeByte(uint8_t *out, ReadError *err) {
        return readSliceAll(out, 1, err);
    }

    /* std.Io.Reader.discardAll, served through the buffer like any read. */
    bool discardAll(size_t n, ReadError *err) {
        uint8_t scratch[64];
        while (n > 0) {
            const size_t step = n < sizeof scratch ? n : sizeof scratch;
            if (!readSliceAll(scratch, step, err)) return false;
            n -= step;
        }
        return true;
    }
};

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_READER_HPP */
