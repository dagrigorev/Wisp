/* Transliterated from Ghostty src/terminal/kitty/clipboard_write.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Kitty clipboard protocol (OSC 5522) write transactions: the
 * stateful accumulation of wdata chunks and walias aliases until the
 * commit packet arrives. See clipboard.zig for the protocol overview.
 *
 * Wisp, differences in shape rather than behavior:
 *   - The error unions are DataError / AliasError / CommitError returns,
 *     with results through out parameters.
 *   - std.ArrayListUnmanaged is zigstd::ArrayListUnmanaged.
 *   - log.warn has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_CLIPBOARD_WRITE_HPP
#define WISP_TERMINAL_KITTY_CLIPBOARD_WRITE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../simd/base64.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/array_list.hpp"
#include "../clipboard.hpp"
#include "clipboard_command.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace clipboard {

/* Default maximum total decoded bytes accumulated by one write
 * transaction. Embedders can override this per-transaction via
 * WriteState.Options (Ghostty exposes it as the
 * `clipboard-write-limit-bytes` configuration). */
static const size_t max_write_size = 64 * 1024 * 1024;

/* Maximum MIME types and aliases per write transaction. */
static const size_t max_write_mimes = 64;
static const size_t max_write_aliases = 64;

/* One MIME representation of committed clipboard data. This is the
 * same type the clipboard write effect consumes so committed contents
 * can be passed through directly. */
typedef ::wisp::terminal::clipboard::Content Content;

/* The state of one in-flight write transaction: a single `type=write`
 * plus all the `wdata` chunks and `walias` aliases until completion
 * or error. */
struct WriteState {
    struct Options {
        /* Maximum total decoded bytes accumulated by the transaction. */
        size_t max_size; /* = max_write_size */

        Options() : max_size(max_write_size) {}
    };

    struct Entry {
        /* Owned by the transaction arena. */
        ZStr mime;
        size_t start; /* = 0 */
        size_t len;   /* = 0 */

        Entry() : mime(), start(0), len(0) {}
    };

    struct Alias {
        /* Owned by the transaction arena. */
        ZStr alias;
        ZStr target;

        Alias() : alias(), target() {}
    };

    /* Wisp: the error sets of data / alias / commit. */
    enum class DataError : uint8_t { none, OutOfMemory, TooLarge, Invalid };
    enum class AliasError : uint8_t { none, OutOfMemory, Invalid };
    enum class CommitError : uint8_t { none, OutOfMemory, Invalid };

    zigstd::ArenaAllocator arena;
    ::wisp::terminal::clipboard::Location loc;
    ZStr id;
    ZStr pw;
    ZStr name;
    zigstd::ArrayListUnmanaged<uint8_t> spool;
    zigstd::ArrayListUnmanaged<Entry> entries;
    zigstd::ArrayListUnmanaged<Alias> aliases;

    /* Maximum total decoded bytes this transaction will accumulate.
     * Captured at init so a change to the configured limit doesn't
     * apply to a transaction already in flight. */
    size_t max_size;

    /* Index into entries currently receiving data. */
    bool has_current;
    size_t current; /* = null */

    /* Decodes the concatenated payload stream of the entry currently
     * receiving data. Per the spec's "Encoding of payloads" section,
     * individual wdata packet payloads split one base64 stream at
     * arbitrary boundaries; only the concatenation per MIME type must
     * be valid, correctly padded base64. */
    simd::base64::Streaming decoder;

    explicit WriteState(zigstd::Allocator alloc)
        : arena(alloc), loc(::wisp::terminal::clipboard::Location::standard), id(), pw(), name(), spool(),
          entries(), aliases(), max_size(max_write_size), has_current(false), current(0), decoder() {}

    /* Wisp: arena.allocator().dupe of a ZStr. */
    bool arenaDupe(ZStr src, ZStr *out) {
        if (src.len == 0) {
            *out = ZStr();
            return true;
        }
        uint8_t *p = arena.allocator().allocT<uint8_t>(src.len);
        if (p == nullptr) return false;
        memcpy(p, src.ptr, src.len);
        *out = ZStr((const char *)p, src.len);
        return true;
    }

    /* Begin a transaction from a type=write packet. */
    static bool init(zigstd::Allocator alloc, const Metadata *meta, Options opts, WriteState *out) {
        /* assert(meta.op == .write) */
        WriteState self(alloc);
        if (!self.arenaDupe(meta->id, &self.id) || !self.arenaDupe(meta->pw, &self.pw) ||
            !self.arenaDupe(meta->name, &self.name)) {
            self.arena.deinit();
            return false;
        }
        self.loc = meta->loc;
        self.max_size = opts.max_size;
        *out = self;
        return true;
    }

    void deinit(zigstd::Allocator alloc) {
        spool.deinit(alloc);
        entries.deinit(alloc);
        aliases.deinit(alloc);
        arena.deinit();
    }

    /* Finish the decode stream of the entry currently receiving
     * data. Returns error.Invalid when the stream ends mid-group,
     * i.e. the concatenated payload was not correctly padded. */
    bool finishCurrent() { return decoder.finish(); }

    /* Accumulate one wdata chunk carrying data for meta.mime (which
     * must be non-empty; an empty mime is a commit, not data).
     *
     * Returns error.TooLarge when the transaction exceeds max_size
     * and error.Invalid when the payload stream is not valid base64.
     * The caller must fail the whole transaction with EFBIG or EINVAL
     * respectively and abort it, as required by the protocol. */
    DataError data(zigstd::Allocator alloc, const Metadata *meta, ZStr payload) {
        /* assert(meta.op == .wdata); assert(meta.mime.len > 0) */

        /* Switch the receiving entry if this chunk is for a different
         * MIME type than the last one. */
        bool entry_done = false;
        if (has_current) {
            Entry *entry = &entries.items[current];
            if (entry->mime.len == meta->mime.len && memcmp(entry->mime.ptr, meta->mime.ptr, entry->mime.len) == 0) {
                entry_done = true;
            } else {
                /* Finalize the previous region. Its concatenated
                 * stream must have ended on a complete base64 group,
                 * otherwise the data was not correctly padded. */
                if (!finishCurrent()) return DataError::Invalid;
                entry->len = spool.items_len - entry->start;
            }
        }

        if (!entry_done) {
            /* Re-using an earlier MIME type starts a fresh region,
             * overwriting the previous mapping. */
            for (size_t idx = 0; idx < entries.items_len; idx++) {
                Entry *entry = &entries.items[idx];
                if (entry->mime.len == meta->mime.len &&
                    memcmp(entry->mime.ptr, meta->mime.ptr, entry->mime.len) == 0) {
                    entry->start = spool.items_len;
                    entry->len = 0;
                    has_current = true;
                    current = idx;
                    entry_done = true;
                    break;
                }
            }
        }

        if (!entry_done) {
            if (entries.items_len >= max_write_mimes) {
                /* log.warn("clipboard write has too many MIME types, ignoring mime={s}") */
                has_current = false;
                return DataError::none;
            }

            Entry e;
            if (!arenaDupe(meta->mime, &e.mime)) return DataError::OutOfMemory;
            e.start = spool.items_len;
            if (!entries.append(alloc, e)) return DataError::OutOfMemory;
            has_current = true;
            current = entries.items_len - 1;
        }

        /* The payloads for one MIME region concatenate into a single
         * strict base64 stream, decoded directly into the spool's
         * unused capacity. Invalid data aborts the transaction; per
         * the spec it must not be silently discarded "since that
         * turns corrupted data into apparently valid data". */
        if (!spool.ensureUnusedCapacity(alloc, decoder.maxLen(payload.len))) return DataError::OutOfMemory;
        size_t decoded_len = 0;
        if (!decoder.feed((const uint8_t *)payload.ptr, payload.len, spool.unusedCapacitySlice(),
                          spool.unusedCapacityLen(), &decoded_len))
            return DataError::Invalid;

        /* The limit covers all decoded data in the transaction. Going
         * over it aborts the entire write; partial clipboard contents
         * must never reach the embedder. */
        const size_t remaining = max_size > spool.items_len ? max_size - spool.items_len : 0;
        if (decoded_len > remaining) return DataError::TooLarge;
        spool.items_len += decoded_len;
        return DataError::none;
    }

    /* Register aliases from a walias packet: meta.mime is the target
     * (the type that carries data) and the payload is a base64-encoded,
     * whitespace-separated list of aliases. Returns error.Invalid for
     * an undecodable payload, which aborts the transaction with EINVAL. */
    AliasError alias(zigstd::Allocator alloc, const Metadata *meta, ZStr payload) {
        /* assert(meta.op == .walias); assert(meta.mime.len > 0) */
        Payload decoded;
        switch (Payload::init(alloc, payload, &decoded)) {
        case Payload::InitError::none: break;
        case Payload::InitError::OutOfMemory: return AliasError::OutOfMemory;
        case Payload::InitError::Invalid: return AliasError::Invalid;
        }
        struct PayloadGuard {
            const Payload *p;
            zigstd::Allocator a;
            ~PayloadGuard() { p->deinit(a); }
        } payload_guard = {&decoded, alloc};
        (void)payload_guard;

        if (!decoded.isValidUtf8()) return AliasError::Invalid;
        Payload::MimeIterator it = decoded.mimeIterator();

        /* Copy the target only if at least one valid alias exists. */
        ZStr target;
        {
            bool found = false;
            ZStr name_;
            while (it.next(&name_)) {
                if (name_.len > max_mime_len) continue;
                if (!arenaDupe(meta->mime, &target)) return AliasError::OutOfMemory;
                found = true;
                break;
            }

            /* If we didn't find a target then ignore it. */
            if (!found) return AliasError::none;
        }

        /* Rewind so the alias that satisfied the check above is
         * associated too. */
        it.reset();

        /* Associate the aliases */
        ZStr name_;
        while (it.next(&name_)) {
            if (name_.len > max_mime_len) continue;

            /* A repeated alias overwrites its previous target. */
            bool replaced = false;
            for (size_t i = 0; i < aliases.items_len; i++) {
                Alias *a = &aliases.items[i];
                if (a->alias.len == name_.len && memcmp(a->alias.ptr, name_.ptr, name_.len) == 0) {
                    a->target = target;
                    replaced = true;
                    break;
                }
            }
            if (replaced) continue;

            if (aliases.items_len >= max_write_aliases) {
                /* log.warn("clipboard write has too many aliases, ignoring") */
                return AliasError::none;
            }

            Alias a;
            if (!arenaDupe(name_, &a.alias)) return AliasError::OutOfMemory;
            a.target = target;
            if (!aliases.append(alloc, a)) return AliasError::OutOfMemory;
        }
        return AliasError::none;
    }

    /* The result of a committed transaction. All slices borrow the
     * WriteState's memory and are valid until it is deinited. */
    struct Committed {
        ::wisp::terminal::clipboard::Location loc;
        ZStr id;
        ZStr pw;
        ZStr name;
        Content *contents;
        size_t contents_len;

        Committed()
            : loc(::wisp::terminal::clipboard::Location::standard), id(), pw(), name(), contents(nullptr),
              contents_len(0) {}

        void deinit(zigstd::Allocator alloc) const {
            if (contents != nullptr) alloc.freeT<Content>(contents, contents_len);
        }
    };

    /* Commit the transaction (a wdata packet without a MIME type).
     * The caller must use the result, call Committed.deinit, and then
     * deinit this state. Returns error.Invalid when the last region's
     * concatenated payload was not correctly padded; the caller must
     * fail the transaction with EINVAL and abort it. */
    CommitError commit(zigstd::Allocator alloc, Committed *out) {
        /* Finalize the region receiving data. */
        if (has_current) {
            if (!finishCurrent()) return CommitError::Invalid;
            Entry *entry = &entries.items[current];
            entry->len = spool.items_len - entry->start;
            has_current = false;
        }

        /* Resolve the final MIME map: entries in arrival order, then
         * aliases applied sequentially against the evolving map so
         * chained aliases work like kitty's dict iteration. An alias
         * whose target has no mapping is dropped; an alias colliding
         * with an existing name overwrites it. */
        zigstd::ArrayListUnmanaged<Content> contents;
        if (!contents.ensureTotalCapacity(alloc, entries.items_len + aliases.items_len)) {
            contents.deinit(alloc);
            return CommitError::OutOfMemory;
        }

        for (size_t i = 0; i < entries.items_len; i++) {
            Entry *entry = &entries.items[i];
            contents.appendAssumeCapacity(
                Content(entry->mime, ZStr((const char *)(spool.items + entry->start), entry->len)));
        }

        for (size_t i = 0; i < aliases.items_len; i++) {
            Alias *a = &aliases.items[i];
            Content target;
            bool found = false;
            for (size_t j = 0; j < contents.items_len; j++) {
                const Content &c = contents.items[j];
                if (c.mime.len == a->target.len && memcmp(c.mime.ptr, a->target.ptr, c.mime.len) == 0) {
                    target = c;
                    found = true;
                    break;
                }
            }
            if (!found) continue;

            bool replaced = false;
            for (size_t j = 0; j < contents.items_len; j++) {
                Content *c = &contents.items[j];
                if (c->mime.len == a->alias.len && memcmp(c->mime.ptr, a->alias.ptr, c->mime.len) == 0) {
                    c->data = target.data;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) contents.appendAssumeCapacity(Content(a->alias, target.data));
        }

        Committed result;
        result.loc = loc;
        result.id = id;
        result.pw = pw;
        result.name = name;
        if (!contents.toOwnedSlice(alloc, &result.contents, &result.contents_len)) {
            contents.deinit(alloc);
            return CommitError::OutOfMemory;
        }
        *out = result;
        return CommitError::none;
    }
};

} /* namespace clipboard */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_CLIPBOARD_WRITE_HPP */
