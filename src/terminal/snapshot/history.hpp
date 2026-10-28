/* Transliterated from Ghostty src/terminal/snapshot/history.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * HISTORY record payload encoding.
 *
 * One HISTORY record describes the complete history for a previously
 * encoded screen. It is followed immediately by the number of complete PAGE
 * records declared by `page_count`. Those pages contain the history older
 * than the first page in the corresponding SCREEN sequence and are ordered
 * from newest to oldest.
 *
 * Every encoded SCREEN has one corresponding HISTORY record. A screen with
 * no older pages uses a zero page count and has no following PAGE records.
 *
 * The first SCREEN page may begin above the active area. Those incidental
 * history rows are already present at READY and are not repeated after
 * HISTORY. The corresponding SCREEN header declares the complete logical
 * history extent, including both that resident overlap and the following
 * PAGE records.
 *
 * Newest-to-oldest order lets a reader start showing most-recent history as
 * soon as possible which is more useful to a user.
 *
 * All integers are unsigned and little-endian. The HISTORY payload is only
 * a fixed header: screen key u16, following page count u32. A decoder uses
 * `page_count`, rather than another record tag, to find the end of the page
 * sequence.
 *
 * Wisp, differences in shape rather than behavior:
 *   - Error sets flatten into one enum, `Error`.
 *   - `Decoder` keeps its source reader between `init` and `decode`, so it
 *     is a template over the source type where upstream stores an interface
 *     pointer.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_HISTORY_HPP
#define WISP_TERMINAL_SNAPSHOT_HISTORY_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "../../vt/page.hpp"
#include "../../vt/page_list.hpp"
#include "../../vt/screen.hpp"
#include "../../vt/screen_set.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "io.hpp"
#include "page.hpp"
#include "record.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace history {

typedef ::wisp::vt::Screen TerminalScreen;
typedef ::wisp::vt::PageList TerminalPageList;
typedef ::wisp::vt::ScreenSet::Key TerminalScreenKey;
typedef ::wisp::vt::page::Page TerminalPage;
typedef ::wisp::zigstd::ReadError ReadError;
typedef ::wisp::zigstd::Allocator Allocator;

enum class Error : uint8_t {
    none = 0,

    /* std.Io.Reader.Error / std.Io.Writer.Error */
    EndOfStream,
    ReadFailed,
    WriteFailed,

    /* Header.DecodeError */
    InvalidKey,

    /* The complete historical prefix has more pages than the header fits. */
    PageCountOverflow,

    /* page.EncodeError / page.DecodeError / record errors */
    InvalidUri,
    InvalidExplicitId,
    StringTooLong,
    InvalidWideCell,
    TooManyGraphemes,
    PayloadTooLarge,
    InvalidDimensions,
    InvalidRowCellCount,
    InvalidKind,
    OutOfMemory,
    InvalidDestinationCapacity,
    InvalidTag,
    InvalidChecksum,
    PayloadNotExhausted,
    IntegrityViolation,
    UnexpectedRecordTag,

    /* TerminalPageList.PageAllocation.FinalizeError */
    InvalidPageDimensions,
    RowCountOverflow,
    PageSizeOverflow,
    MaxSizeExceeded,
    MaxLinesExceeded,

    /* The Screen already contains complete pages before its active page. */
    ExistingHistory,

    /* The HISTORY key does not match the caller-selected screen. */
    UnexpectedScreenKey,
};

inline Error fromRead(ReadError e) {
    switch (e) {
        case ReadError::none: return Error::none;
        case ReadError::EndOfStream: return Error::EndOfStream;
        case ReadError::ReadFailed: return Error::ReadFailed;
    }
    return Error::EndOfStream;
}

inline Error fromPage(page::Error e) {
    switch (e) {
        case page::Error::none: return Error::none;
        case page::Error::EndOfStream: return Error::EndOfStream;
        case page::Error::ReadFailed: return Error::ReadFailed;
        case page::Error::WriteFailed: return Error::WriteFailed;
        case page::Error::InvalidUri: return Error::InvalidUri;
        case page::Error::InvalidExplicitId: return Error::InvalidExplicitId;
        case page::Error::StringTooLong: return Error::StringTooLong;
        case page::Error::InvalidWideCell: return Error::InvalidWideCell;
        case page::Error::TooManyGraphemes: return Error::TooManyGraphemes;
        case page::Error::PayloadTooLarge: return Error::PayloadTooLarge;
        case page::Error::InvalidDimensions: return Error::InvalidDimensions;
        case page::Error::InvalidRowCellCount: return Error::InvalidRowCellCount;
        case page::Error::InvalidKind: return Error::InvalidKind;
        case page::Error::OutOfMemory: return Error::OutOfMemory;
        case page::Error::InvalidDestinationCapacity: return Error::InvalidDestinationCapacity;
        case page::Error::InvalidTag: return Error::InvalidTag;
        case page::Error::InvalidChecksum: return Error::InvalidChecksum;
        case page::Error::PayloadNotExhausted: return Error::PayloadNotExhausted;
        case page::Error::IntegrityViolation: return Error::IntegrityViolation;
        case page::Error::UnexpectedRecordTag: return Error::UnexpectedRecordTag;
    }
    return Error::EndOfStream;
}

inline Error fromFinalize(TerminalPageList::PageAllocation::FinalizeError e) {
    typedef TerminalPageList::PageAllocation::FinalizeError F;
    switch (e) {
        case F::none: return Error::none;
        case F::InvalidPageDimensions: return Error::InvalidPageDimensions;
        case F::RowCountOverflow: return Error::RowCountOverflow;
        case F::PageSizeOverflow: return Error::PageSizeOverflow;
        case F::MaxSizeExceeded: return Error::MaxSizeExceeded;
        case F::MaxLinesExceeded: return Error::MaxLinesExceeded;
    }
    return Error::OutOfMemory;
}

/* The complete fixed payload of one HISTORY record. */
struct Header {
    /* Number of encoded bytes in the fixed payload. This size is part of the
     * wire format. If it changes, the snapshot version must also change. */
    static const size_t len = 6;

    /* Identifies the previously encoded screen that owns this history. */
    TerminalScreenKey key = TerminalScreenKey::primary;

    /* Number of complete PAGE records immediately following this record. */
    uint32_t page_count = 0;

    bool operator==(const Header &o) const { return key == o.key && page_count == o.page_count; }

    /* Encode the fixed HISTORY payload. */
    void encode(std::string *writer) const {
        io::writeInt<uint16_t>(writer, (uint16_t)key);
        io::writeInt<uint32_t>(writer, page_count);
    }

    /* Decode and validate the fixed HISTORY payload. */
    template <typename ReaderT>
    static Error decode(ReaderT *reader, Header *out) {
        ReadError rerr = ReadError::none;
        uint16_t raw = 0;
        if (!io::readInt<uint16_t>(reader, &raw, &rerr)) return fromRead(rerr);
        if (raw > (uint16_t)TerminalScreenKey::alternate) return Error::InvalidKey;
        Header h;
        h.key = (TerminalScreenKey)raw;
        if (!io::readInt<uint32_t>(reader, &h.page_count, &rerr)) return fromRead(rerr);
        *out = h;
        return Error::none;
    }
};

/* Encode one screen's HISTORY and its complete historical pages.
 *
 * Pages are encoded newest-to-oldest. Compressed source pages are inspected
 * without changing their storage state. Completed records may already be
 * emitted if a later page fails. */
inline Error encode(const TerminalScreen *terminal_screen, TerminalScreenKey key, record::Writer *destination) {
    /* SCREEN begins at the page containing the active area's first row. Its
     * leading rows are already resident; every previous complete page
     * belongs to this HISTORY sequence. */
    TerminalPageList::Node *first =
        terminal_screen->pages.getTopLeft(::wisp::vt::point::Tag::active).node->prev;
    size_t page_count = 0;
    for (TerminalPageList::Node *node = first; node != nullptr; node = node->prev) page_count += 1;
    if (page_count > 0xFFFFFFFFull) return Error::PageCountOverflow;

    Header header;
    header.key = key;
    header.page_count = (uint32_t)page_count;

    /* HISTORY declares exactly how many PAGE records follow. */
    {
        std::string *payload = destination->begin(record::Tag::history);
        header.encode(payload);
        switch (destination->finish()) {
            case record::Writer::FinishError::none: break;
            case record::Writer::FinishError::WriteFailed: return Error::WriteFailed;
            case record::Writer::FinishError::PayloadTooLarge: return Error::PayloadTooLarge;
        }
    }

    /* Walk backward so each page can be prepended by the decoder
     * immediately. PreservedPage borrows resident pages and clones
     * compressed pages without changing the source node's representation. */
    for (TerminalPageList::Node *node = first; node != nullptr; node = node->prev) {
        TerminalPageList::Node::PreservedPage preserved;
        if (!node->pagePreservingState(terminal_screen->alloc, &preserved)) return Error::OutOfMemory;
        const Error err = fromPage(page::encode(preserved.page(), destination));
        preserved.deinit();
        if (err != Error::none) return err;
    }
    return Error::none;
}

inline bool hasSemanticPrompt(const TerminalPage *terminal_page) {
    for (size_t y = 0; y < terminal_page->size.rows; y++) {
        const ::wisp::vt::page::Row *row = terminal_page->getRow(y);
        if (row->semantic_prompt() != ::wisp::vt::page::Row::SemanticPrompt::none) return true;

        const ::wisp::vt::page::Cell *cells = terminal_page->getCells(row);
        for (size_t x = 0; x < terminal_page->size.cols; x++) {
            if (cells[x].semantic_content() == ::wisp::vt::page::Cell::SemanticContent::prompt) return true;
        }
    }
    return false;
}

/* Restore the next history PAGE record and prepend it to the native Screen.
 *
 * Returns the number of rows added above the screen's existing content. The
 * page is decoded directly into a detached PageList-pooled allocation and
 * prepended only after its record validates, so a failure leaves the screen
 * unchanged. A limit failure from `finalize` occurs after the record bytes
 * were fully consumed, leaving `source` aligned on the next record. */
template <typename SourceT>
inline Error decodePage(SourceT *source, Allocator alloc, TerminalScreen *terminal_screen, size_t *rows_out) {
    /* PAGE exposes its exact capacity before decoding the payload, allowing
     * the destination PageList to allocate the final backing memory once. */
    page::Decoder decoder;
    Error err = fromPage(decoder.init(source));
    if (err != Error::none) return err;
    TerminalPageList::PageAllocation allocation;
    if (!terminal_screen->pages.allocatePage(decoder.capacity(), &allocation)) return Error::OutOfMemory;
    struct AllocationGuard {
        TerminalPageList::PageAllocation *a;
        ~AllocationGuard() { a->deinit(); }
    } allocation_guard = { &allocation };
    err = fromPage(decoder.decode(allocation.page(), alloc));
    if (err != Error::none) return err;

    const size_t rows = allocation.page()->size.rows;
    const bool contains_prompt = hasSemanticPrompt(allocation.page());
    err = fromFinalize(allocation.finalize(TerminalPageList::PageAllocation::Location::prepend));
    if (err != Error::none) return err;
    if (contains_prompt) terminal_screen->semantic_prompt.seen = true;
    if (rows_out != nullptr) *rows_out = rows;
    return Error::none;
}

/* A decoded HISTORY manifest ready to restore its following PAGE records.
 *
 * Keeping manifest decoding separate lets the full snapshot wrapper route a
 * HISTORY sequence by its encoded key before any pages mutate a Screen. */
template <typename SourceT>
struct Decoder {
    SourceT *source = nullptr;
    Header header;

    /* Decode and finish the self-contained HISTORY manifest. */
    Error init(SourceT *src) {
        record::Reader record_reader;
        record::Reader::InitError ierr = record::Reader::InitError::none;
        if (!record_reader.init(src, &ierr)) {
            return ierr == record::Reader::InitError::InvalidTag ? Error::InvalidTag : Error::EndOfStream;
        }
        if (record_reader.header.tag != record::Tag::history) return Error::UnexpectedRecordTag;
        Header h;
        const Error err = Header::decode(record_reader.payloadReader(), &h);
        if (err != Error::none) return err;
        switch (record_reader.finish()) {
            case record::Reader::FinishError::none: break;
            case record::Reader::FinishError::InvalidChecksum: return Error::InvalidChecksum;
            case record::Reader::FinishError::PayloadNotExhausted: return Error::PayloadNotExhausted;
        }
        source = src;
        header = h;
        return Error::none;
    }

    /* Restore the declared PAGE records into the selected native Screen. */
    Error decode(Allocator alloc, TerminalScreen *terminal_screen) {
        /* A freshly restored SCREEN may carry overlap inside its first page,
         * but cannot already contain a complete historical page before that
         * boundary. */
        const TerminalPageList::Pin active_top = terminal_screen->pages.getTopLeft(::wisp::vt::point::Tag::active);
        if (terminal_screen->pages.getTopLeft(::wisp::vt::point::Tag::screen).node != active_top.node) {
            return Error::ExistingHistory;
        }

        /* Native row totals remain derived from the actual PAGE dimensions. */
        for (uint32_t i = 0; i < header.page_count; i++) {
            const Error err = decodePage(source, alloc, terminal_screen, nullptr);
            if (err != Error::none) return err;
        }

        terminal_screen->pages.assertIntegrity();
        terminal_screen->assertIntegrity();
        return Error::none;
    }
};

/* Restore one HISTORY and its declared PAGE records into a native Screen.
 *
 * Each PAGE is decoded directly into a detached PageList-pooled allocation
 * and prepended only after its record and native integrity are validated. If
 * a later PAGE fails, earlier successful pages remain as a contiguous recent
 * history prefix. */
template <typename SourceT>
inline Error decode(SourceT *source, Allocator alloc, TerminalScreenKey expected_key,
                    TerminalScreen *terminal_screen) {
    Decoder<SourceT> decoder;
    const Error err = decoder.init(source);
    if (err != Error::none) return err;
    if (decoder.header.key != expected_key) return Error::UnexpectedScreenKey;
    return decoder.decode(alloc, terminal_screen);
}

} /* namespace history */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_HISTORY_HPP */
