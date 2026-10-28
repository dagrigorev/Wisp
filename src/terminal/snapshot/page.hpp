/* Transliterated from Ghostty src/terminal/snapshot/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * PAGE record payload encoding.
 *
 * One PAGE record represents a set of rows/columns in the terminal. In
 * libghostty, this happens to map internally to a very specific data
 * structure called a "page" (hence the name), but consumers of the format
 * don't need to reproduce that.
 *
 * The important thing is that one page is fully self-contained: it has a
 * dimension (cols x rows), a set of styles, hyperlinks, cells, etc. and
 * depends on no external state to decode that with some exceptions like
 * assets such as images.
 *
 * A page could have a dimension that doesn't match the terminal dimensions,
 * e.g. for lazy resize/reflow. Callers must be prepared for that.
 *
 * ## Binary Format
 *
 * Every PAGE payload begins with a fixed header, followed by a payload of
 * styles, hyperlinks, and rows and columns. All integers are unsigned and
 * little-endian.
 *
 * ### Header
 *
 * | Offset | Size | Field                              |
 * | -----: | ---: | :--------------------------------- |
 * |      0 |    2 | Logical columns (`u16`)            |
 * |      2 |    2 | Logical rows (`u16`)               |
 * |      4 |    2 | Non-default style count (`u16`)    |
 * |      6 |    2 | Unique hyperlink count (`u16`)     |
 * |      8 |    2 | Style capacity hint (`u16`)        |
 * |     10 |    2 | Hyperlink capacity bytes (`u16`)   |
 * |     12 |    4 | Grapheme capacity bytes (`u32`)    |
 * |     16 |    4 | String capacity bytes (`u32`)      |
 *
 * The style count and hyperlink count denote the number of styles and
 * hyperlinks sent with the page. The default style and absence of a
 * hyperlink are implicit at native ID zero and are not included in these
 * counts. Each encoded table entry carries its nonzero native page ID.
 *
 * The final four fields are allocation hints copied from the source page.
 *
 * ### Payload
 *
 * Header, then `style_count` style entries (ID `u16` + style.zig record),
 * then `hyperlink_count` hyperlink entries (ID `u16` + hyperlink.zig
 * record), then the grid (grid.zig). No padding between records. Style
 * and hyperlink IDs are separate namespaces, in no specific order, and may
 * contain gaps.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Error sets become enums; results are out-params beside a bool or an
 *     error return. `TerminalPage.IntegrityError`'s members collapse into
 *     `IntegrityViolation`.
 *   - Upstream decodes in place when the record payload is already fully
 *     buffered. The port's readers do not expose their buffer, so decode
 *     uses upstream's other two branches: stage payloads up to 8 MiB into
 *     one allocation, stream anything larger.
 *   - A style table entry is read as one 18-byte slice and parsed with
 *     `parseOrNull`, which is upstream's buffered branch. It propagates the
 *     reader's own error, `ReadFailed` included.
 *   - `encode`'s `errdefer destination.cancel()` is explicit.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_PAGE_HPP
#define WISP_TERMINAL_SNAPSHOT_PAGE_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../vt/page.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "grid.hpp"
#include "hyperlink.hpp"
#include "io.hpp"
#include "record.hpp"
#include "style.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace page {

namespace terminal_hyperlink = ::wisp::vt::hyperlink;
namespace terminal_page = ::wisp::vt::page;
namespace terminal_style = ::wisp::vt::style;

typedef terminal_hyperlink::Hyperlink TerminalHyperlink;
typedef terminal_hyperlink::Id TerminalHyperlinkId;
typedef terminal_hyperlink::PageEntry TerminalHyperlinkPageEntry;
typedef terminal_page::Cell TerminalCell;
typedef terminal_page::Page TerminalPage;
typedef terminal_page::Capacity TerminalPageCapacity;
typedef terminal_page::Row TerminalRow;
typedef terminal_style::Style TerminalStyle;
typedef terminal_style::Id TerminalStyleId;
typedef ::wisp::zigstd::ReadError ReadError;
typedef ::wisp::zigstd::Allocator Allocator;

/* Every PAGE error, flattened. */
enum class Error : uint8_t {
    none = 0,

    /* std.Io.Reader.Error */
    EndOfStream,
    ReadFailed,

    /* std.Io.Writer.Error. Wisp: unreachable with a std::string writer. */
    WriteFailed,

    /* hyperlink.EncodeError */
    InvalidUri,
    InvalidExplicitId,
    StringTooLong,

    /* grid.EncodeError */
    InvalidWideCell,
    TooManyGraphemes,

    /* record.Writer.FinishError */
    PayloadTooLarge,

    /* Header.CapacityError */
    InvalidDimensions,

    /* grid.DecodeError */
    InvalidRowCellCount,

    /* The hyperlink kind is not defined by snapshot version 1. */
    InvalidKind,

    /* Native page backing memory could not be allocated. */
    OutOfMemory,

    /* The caller-provided page does not have the advertised capacity. */
    InvalidDestinationCapacity,

    /* record.Reader.InitError */
    InvalidTag,

    /* record.Reader.FinishError */
    InvalidChecksum,
    PayloadNotExhausted,

    /* TerminalPage.IntegrityError, collapsed. */
    IntegrityViolation,

    /* The next record is valid but is not a PAGE. */
    UnexpectedRecordTag,
};

inline Error fromRead(ReadError e) {
    switch (e) {
        case ReadError::none: return Error::none;
        case ReadError::EndOfStream: return Error::EndOfStream;
        case ReadError::ReadFailed: return Error::ReadFailed;
    }
    return Error::EndOfStream;
}

/* The fixed logical dimensions, table counts, and allocation hints at the
 * start of PAGE. */
struct Header {
    /* Number of bytes written by `encode`. This size is part of the wire
     * format. If it changes, the snapshot version and golden fixtures must
     * also change. */
    static const size_t len = 20;

    /* Number of logical cells in every encoded row. */
    uint16_t columns = 0;

    /* Number of logical rows encoded after the tables. */
    uint16_t rows = 0;

    /* Number of non-default style entries following the header. */
    uint16_t style_count = 0;

    /* Number of unique hyperlink entries following the style table. */
    uint16_t hyperlink_count = 0;

    /* Suggested capacity for the native non-default style set. */
    uint16_t style_capacity = 0;

    /* Suggested native hyperlink storage capacity in bytes. */
    uint16_t hyperlink_capacity_bytes = 0;

    /* Suggested native grapheme storage capacity in bytes. */
    uint32_t grapheme_capacity_bytes = 0;

    /* Suggested native string storage capacity in bytes. */
    uint32_t string_capacity_bytes = 0;

    bool operator==(const Header &o) const {
        return columns == o.columns && rows == o.rows && style_count == o.style_count &&
               hyperlink_count == o.hyperlink_count && style_capacity == o.style_capacity &&
               hyperlink_capacity_bytes == o.hyperlink_capacity_bytes &&
               grapheme_capacity_bytes == o.grapheme_capacity_bytes &&
               string_capacity_bytes == o.string_capacity_bytes;
    }

    /* Encode the fixed PAGE payload header. */
    void encode(std::string *writer) const {
        io::writeInt<uint16_t>(writer, columns);
        io::writeInt<uint16_t>(writer, rows);
        io::writeInt<uint16_t>(writer, style_count);
        io::writeInt<uint16_t>(writer, hyperlink_count);
        io::writeInt<uint16_t>(writer, style_capacity);
        io::writeInt<uint16_t>(writer, hyperlink_capacity_bytes);
        io::writeInt<uint32_t>(writer, grapheme_capacity_bytes);
        io::writeInt<uint32_t>(writer, string_capacity_bytes);
    }

    /* Decode the fixed PAGE payload header.
     *
     * This reads field values only. The complete PAGE decoder is responsible
     * for applying configured limits before using any capacity hint. */
    template <typename ReaderT>
    static bool decode(ReaderT *reader, Header *out, ReadError *err) {
        Header h;
        if (!io::readInt<uint16_t>(reader, &h.columns, err)) return false;
        if (!io::readInt<uint16_t>(reader, &h.rows, err)) return false;
        if (!io::readInt<uint16_t>(reader, &h.style_count, err)) return false;
        if (!io::readInt<uint16_t>(reader, &h.hyperlink_count, err)) return false;
        if (!io::readInt<uint16_t>(reader, &h.style_capacity, err)) return false;
        if (!io::readInt<uint16_t>(reader, &h.hyperlink_capacity_bytes, err)) return false;
        if (!io::readInt<uint32_t>(reader, &h.grapheme_capacity_bytes, err)) return false;
        if (!io::readInt<uint32_t>(reader, &h.string_capacity_bytes, err)) return false;
        *out = h;
        return true;
    }

    /* Initialize a header from the current contents of a native page.
     *
     * This copies the page's existing allocation capacities. It does not
     * scan cells, allocate, or encode any bytes. */
    static Header init(const TerminalPage *page) {
        Header h;
        h.columns = page->size.cols;
        h.rows = page->size.rows;
        h.style_count = (uint16_t)page->styles.count();
        h.hyperlink_count = (uint16_t)page->hyperlink_set.count();
        h.style_capacity = page->capacity.styles;
        h.hyperlink_capacity_bytes = page->capacity.hyperlink_bytes;
        h.grapheme_capacity_bytes = page->capacity.grapheme_bytes;
        h.string_capacity_bytes = page->capacity.string_bytes;
        return h;
    }

    /* Validate native allocation requirements and produce the page capacity. */
    Error pageCapacity(TerminalPageCapacity *out) const {
        if (columns == 0 || rows == 0) return Error::InvalidDimensions;
        TerminalPageCapacity c;
        c.cols = columns;
        c.rows = rows;
        c.styles = style_capacity;
        c.hyperlink_bytes = hyperlink_capacity_bytes;
        c.grapheme_bytes = grapheme_capacity_bytes;
        c.string_bytes = string_capacity_bytes;
        *out = c;
        return Error::none;
    }
};

inline TerminalHyperlink pageHyperlink(const TerminalPage *page,
                                       const TerminalHyperlinkPageEntry *entry) {
    const void *base = page->memory;
    TerminalHyperlink h;
    switch (entry->id.tag) {
        case TerminalHyperlinkPageEntry::Id::Tag::implicit:
            h.id = TerminalHyperlink::Id::makeImplicit(entry->id.implicit);
            break;
        case TerminalHyperlinkPageEntry::Id::Tag::explicit_:
            h.id = TerminalHyperlink::Id::makeExplicit(entry->id.explicit_.slice(base),
                                                       entry->id.explicit_.len);
            break;
    }
    h.uri = entry->uri.slice(base);
    h.uri_len = entry->uri.len;
    return h;
}

/* Encode a PAGE payload directly from a native page. */
inline Error encodePayload(const TerminalPage *page, std::string *writer) {
    /* Write header */
    Header::init(page).encode(writer);

    /* Sparse styles */
    auto style_it = page->styles.iterator((const void *)page->memory);
    terminal_style::Set::Entry style_entry;
    while (style_it.next(&style_entry)) {
        io::writeInt<TerminalStyleId>(writer, style_entry.id);
        style::encode(*style_entry.value_ptr, writer);
    }

    /* Sparse hyperlinks */
    auto hyperlink_it = page->hyperlink_set.iterator((const void *)page->memory);
    terminal_hyperlink::Set::Entry hyperlink_entry;
    while (hyperlink_it.next(&hyperlink_entry)) {
        io::writeInt<TerminalHyperlinkId>(writer, hyperlink_entry.id);
        switch (hyperlink::encode(pageHyperlink(page, hyperlink_entry.value_ptr), writer)) {
            case hyperlink::EncodeError::none: break;
            case hyperlink::EncodeError::InvalidUri: return Error::InvalidUri;
            case hyperlink::EncodeError::InvalidExplicitId: return Error::InvalidExplicitId;
            case hyperlink::EncodeError::StringTooLong: return Error::StringTooLong;
        }
    }

    /* Rows and cells */
    switch (grid::encode(page, writer)) {
        case grid::EncodeError::none: break;
        case grid::EncodeError::InvalidWideCell: return Error::InvalidWideCell;
        case grid::EncodeError::TooManyGraphemes: return Error::TooManyGraphemes;
    }
    return Error::none;
}

/* Encode one complete PAGE record from a native page.
 *
 * The payload is built in the stream's reusable record buffer. If payload
 * encoding fails, no bytes from this record are emitted. */
inline Error encode(const TerminalPage *page, record::Writer *destination) {
    std::string *payload = destination->begin(record::Tag::page);
    const Error err = encodePayload(page, payload);
    if (err != Error::none) {
        destination->cancel(); /* errdefer */
        return err;
    }
    switch (destination->finish()) {
        case record::Writer::FinishError::none: return Error::none;
        case record::Writer::FinishError::WriteFailed: return Error::WriteFailed;
        case record::Writer::FinishError::PayloadTooLarge: return Error::PayloadTooLarge;
    }
    return Error::none;
}

/* Decode PAGE tables and grid after the header into initialized native page
 * storage with the exact advertised capacity and dimensions. */
template <typename ReaderT>
inline Error decodePayloadBody(ReaderT *reader, Allocator alloc, TerminalPage *page,
                               const Header &header) {
    page->pauseIntegrityChecks(true);
    struct ResumeGuard {
        TerminalPage *p;
        ~ResumeGuard() { p->pauseIntegrityChecks(false); }
    } resume = { page };

    const void *base = page->memory;

    /* Pages without styles or hyperlinks, the common case for plain
     * scrollback, skip the remap tables entirely: every encoded cell ID
     * resolves to the default through the empty remap. */
    grid::StyleRemap style_remap = grid::StyleRemap::empty();
    if (header.style_count > 0 && !grid::StyleRemap::init(alloc, &style_remap)) {
        return Error::OutOfMemory;
    }
    struct StyleGuard {
        grid::StyleRemap *r;
        Allocator a;
        ~StyleGuard() { r->deinit(a); }
    } style_guard = { &style_remap, alloc };

    grid::HyperlinkRemap hyperlink_remap = grid::HyperlinkRemap::empty();
    if (header.hyperlink_count > 0 && !grid::HyperlinkRemap::init(alloc, &hyperlink_remap)) {
        return Error::OutOfMemory;
    }
    struct HyperlinkGuard {
        grid::HyperlinkRemap *r;
        Allocator a;
        ~HyperlinkGuard() { r->deinit(a); }
    } hyperlink_guard = { &hyperlink_remap, alloc };

    /* Styles. The complete fixed-size entry is parsed as one slice so each
     * entry costs one reader call. */
    const size_t style_entry_len = sizeof(TerminalStyleId) + style::len;
    for (size_t i = 0; i < header.style_count; i++) {
        uint8_t bytes[sizeof(TerminalStyleId) + style::len];
        ReadError rerr = ReadError::none;
        if (!reader->readSliceAll(bytes, style_entry_len, &rerr)) return fromRead(rerr);
        const TerminalStyleId native_id = (TerminalStyleId)(bytes[0] | ((TerminalStyleId)bytes[1] << 8));
        TerminalStyle value;
        const bool valid = style::parseOrNull(&bytes[2], &value);

        /* Zero is reserved for the implicit default. For a duplicate encoded
         * ID, the first entry wins and this complete entry is simply ignored. */
        if (native_id == 0 || style_remap.contains(native_id)) continue;

        /* Invalid/default styles map to the native default. `add` returns
         * the existing entry for a repeated concrete value, taking one
         * reference either way, while capacity failure degrades only this
         * style. The references are surrendered through the remap below
         * once every cell reference is installed. */
        TerminalStyleId decoded_id = 0;
        if (valid && !value.default_()) {
            if (page->styles.add(base, value, &decoded_id) != ::wisp::vt::ref_counted_set::AddError::none) {
                decoded_id = 0;
            }
        }
        style_remap.put(native_id, decoded_id);
    }

    /* Hyperlinks */
    for (size_t i = 0; i < header.hyperlink_count; i++) {
        TerminalHyperlinkId native_id = 0;
        ReadError rerr = ReadError::none;
        if (!io::readInt<TerminalHyperlinkId>(reader, &native_id, &rerr)) return fromRead(rerr);
        TerminalHyperlinkId decoded_id = 0;
        hyperlink::DecodePageError herr = hyperlink::DecodePageError::none;
        if (!hyperlink::decodePage(page, reader, &decoded_id, &herr)) {
            return herr == hyperlink::DecodePageError::InvalidKind ? Error::InvalidKind
                                                                   : Error::EndOfStream;
        }

        /* As with styles, zero is reserved and the first duplicate encoded ID
         * wins. Release any native entry reference created for an ignored ID. */
        if (native_id == 0 || hyperlink_remap.contains(native_id)) {
            if (decoded_id != 0) page->hyperlink_set.release(base, decoded_id);
            continue;
        }

        /* Zero records an ignored table entry. Grid decoding treats every
         * reference to it as no hyperlink. */
        hyperlink_remap.put(native_id, decoded_id);
    }

    /* Rows and cells */
    switch (grid::decode(page, reader, &style_remap, &hyperlink_remap)) {
        case grid::DecodeError::none: break;
        case grid::DecodeError::EndOfStream: return Error::EndOfStream;
        case grid::DecodeError::InvalidRowCellCount: return Error::InvalidRowCellCount;
    }

    /* Every accepted table entry took one reference through `add` so grid
     * decoding can safely attach its style to any number of cells. Unlike
     * organically built pages, those references do not themselves represent
     * cells. Release through the encoded-ID remap, so duplicate values
     * which deduplicated to the same native ID each surrender their own
     * reference; unused entries then become dead and disappear from
     * canonical re-encoding. */
    for (size_t encoded_id = 0; encoded_id < style_remap.entries_len; encoded_id++) {
        if (!style_remap.contains((TerminalStyleId)encoded_id)) continue;
        const TerminalStyleId id = style_remap.entries[encoded_id];
        if (id != 0) page->styles.release(base, id);
    }

    /* Hyperlink insertion likewise creates one temporary reference for every
     * accepted wire table entry. Release through the encoded-ID remap rather
     * than the native set so duplicate values which deduplicated to the same
     * native ID each surrender their own reference. */
    for (size_t encoded_id = 0; encoded_id < hyperlink_remap.entries_len; encoded_id++) {
        if (!hyperlink_remap.contains((TerminalHyperlinkId)encoded_id)) continue;
        const TerminalHyperlinkId id = hyperlink_remap.entries[encoded_id];
        if (id != 0) page->hyperlink_set.release(base, id);
    }
    return Error::none;
}

/* Decode a PAGE payload directly into a fresh native page.
 *
 * `alloc` is used only for the temporary native-ID remap tables. Styles,
 * hyperlinks, strings, graphemes, rows, and cells are stored in the page. */
template <typename ReaderT>
inline Error decodePayload(ReaderT *reader, Allocator alloc, TerminalPage *out) {
    Header header;
    ReadError rerr = ReadError::none;
    if (!Header::decode(reader, &header, &rerr)) return fromRead(rerr);
    TerminalPageCapacity capacity;
    const Error cerr = header.pageCapacity(&capacity);
    if (cerr != Error::none) return cerr;
    TerminalPage page;
    if (!TerminalPage::init(capacity, &page)) return Error::OutOfMemory;
    const Error err = decodePayloadBody(reader, alloc, &page, header);
    if (err != Error::none) {
        page.deinit(); /* errdefer */
        return err;
    }
    *out = page;
    return Error::none;
}

inline Error fromInit(record::Reader::InitError e) {
    switch (e) {
        case record::Reader::InitError::none: return Error::none;
        case record::Reader::InitError::EndOfStream: return Error::EndOfStream;
        case record::Reader::InitError::InvalidTag: return Error::InvalidTag;
    }
    return Error::EndOfStream;
}

inline Error fromFinish(record::Reader::FinishError e) {
    switch (e) {
        case record::Reader::FinishError::none: return Error::none;
        case record::Reader::FinishError::InvalidChecksum: return Error::InvalidChecksum;
        case record::Reader::FinishError::PayloadNotExhausted: return Error::PayloadNotExhausted;
    }
    return Error::none;
}

/* PAGE record decoder where the caller is responsible for owning the Page
 * memory. This is particularly useful paired with PageList.Builder so you
 * can build up a proper PageList with valid memory ownership. */
struct Decoder {
    record::Reader record_reader;
    Header header;

    /* Largest remaining PAGE payload staged into one contiguous buffer.
     * This comfortably covers every payload a standard-capacity page can
     * produce while keeping the allocation an untrusted length can force
     * far below the record framing's four-byte length limit. */
    static const size_t max_staged_payload = 8 * 1024 * 1024;

    /* Begin decoding one PAGE record and expose its required capacity.
     * After this, call `capacity` to get the capacity to allocate the page
     * properly, then `decode` into it. */
    template <typename ReaderT>
    Error init(ReaderT *reader) {
        record::Reader::InitError ierr = record::Reader::InitError::none;
        if (!record_reader.init(reader, &ierr)) return fromInit(ierr);
        if (record_reader.header.tag != record::Tag::page) return Error::UnexpectedRecordTag;

        ReadError rerr = ReadError::none;
        if (!Header::decode(record_reader.payloadReader(), &header, &rerr)) return fromRead(rerr);
        TerminalPageCapacity unused;
        return header.pageCapacity(&unused);
    }

    /* Return the exact native capacity advertised by the PAGE header. */
    TerminalPageCapacity capacity() const {
        TerminalPageCapacity c;
        const Error err = header.pageCapacity(&c);
        assert(err == Error::none); /* catch unreachable */
        (void)err;
        return c;
    }

    /* Decode the remaining payload into caller-owned native page storage.
     *
     * The destination must be freshly initialized with `capacity`. `alloc`
     * is used only for temporary staging, ID remaps, and integrity-check
     * storage. */
    Error decode(TerminalPage *destination, Allocator alloc) {
        if (!(destination->capacity == capacity())) return Error::InvalidDestinationCapacity;

        destination->size.cols = header.columns;
        destination->size.rows = header.rows;

        /* `init` consumed exactly the fixed header from the declared payload,
         * so this is the byte count of the tables and grid. */
        const size_t remaining = record_reader.header.payload_len - Header::len;

        if (remaining <= max_staged_payload) {
            /* Stage the payload with one bulk read. The whole payload passes
             * through the checksum hasher as one update and the payload
             * decoders then parse a flat buffer. The CRC and exact-length
             * checks in `finish` are unaffected. */
            uint8_t *staged = alloc.alloc(remaining);
            if (staged == nullptr) return Error::OutOfMemory;
            struct StagedGuard {
                Allocator a;
                uint8_t *p;
                size_t n;
                ~StagedGuard() { a.free(p, n); }
            } staged_guard = { alloc, staged, remaining };

            ReadError rerr = ReadError::none;
            if (!record_reader.payloadReader()->readSliceAll(staged, remaining, &rerr)) {
                return fromRead(rerr);
            }

            ::wisp::zigstd::Reader staged_reader = ::wisp::zigstd::Reader::fixed(staged, remaining);
            const Error err = decodePayloadBody(&staged_reader, alloc, destination, header);
            if (err != Error::none) return err;
            if (staged_reader.remaining() != 0) return Error::PayloadNotExhausted;
        } else {
            /* A payload this large is either hostile or a page far beyond
             * native capacities. Decode it through the streaming payload
             * reader so its declared length cannot force an allocation. */
            const Error err = decodePayloadBody(record_reader.payloadReader(), alloc, destination, header);
            if (err != Error::none) return err;
        }

        const Error ferr = fromFinish(record_reader.finish());
        if (ferr != Error::none) return ferr;

        /* The decoder normalizes every semantic value, so a complete decode
         * upholds native page invariants by construction. Verifying them
         * again is a defense against decoder bugs and follows the native
         * page policy: full integrity verification only when slow runtime
         * safety is enabled. */
        if (::wisp::vt::slow_runtime_safety) {
            if (destination->verifyIntegrity() != terminal_page::PageError::none) {
                return Error::IntegrityViolation;
            }
        }
        return Error::none;
    }
};

/* Decode and validate one complete PAGE record into a fresh native page.
 *
 * The record tag, payload boundary, CRC32C, payload contents, and final page
 * integrity are all validated before the page is returned. `alloc` is used
 * only for temporary decode remaps and integrity-check storage. */
template <typename ReaderT>
inline Error decode(ReaderT *reader, Allocator alloc, TerminalPage *out) {
    Decoder decoder;
    Error err = decoder.init(reader);
    if (err != Error::none) return err;

    TerminalPage page;
    if (!TerminalPage::init(decoder.capacity(), &page)) return Error::OutOfMemory;
    err = decoder.decode(&page, alloc);
    if (err != Error::none) {
        page.deinit(); /* errdefer */
        return err;
    }
    *out = page;
    return Error::none;
}

/* Consume exactly one complete PAGE record, validating its framing and
 * CRC32C while discarding the payload without structural validation.
 *
 * This lets a decoder stay aligned with the record sequence while dropping
 * page content it can no longer apply. */
template <typename ReaderT>
inline Error discard(ReaderT *source) {
    record::Reader record_reader;
    record::Reader::InitError ierr = record::Reader::InitError::none;
    if (!record_reader.init(source, &ierr)) return fromInit(ierr);
    if (record_reader.header.tag != record::Tag::page) return Error::UnexpectedRecordTag;
    ReadError rerr = ReadError::none;
    if (!record_reader.payloadReader()->discardAll(record_reader.header.payload_len, &rerr)) {
        return fromRead(rerr);
    }
    return fromFinish(record_reader.finish());
}

} /* namespace page */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_PAGE_HPP */
