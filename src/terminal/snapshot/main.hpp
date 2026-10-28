/* Transliterated from Ghostty src/terminal/snapshot/main.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Terminal snapshot binary representation and codecs.
 *
 * This is NOT a full transport-ready format to implement generic replay
 * software such as multiplexers, recorders (e.g. asciinema), etc. The goal
 * of this package is to provide a documented, binary-compatible representation
 * for a terminal state.
 *
 * We call this a "snapshot." The snapshot is purposely laid out in a way
 * that prioritizes making a terminal functional as quickly as possible.
 * To do that, it sends the active terminal state followed by a READY record,
 * then complete history.
 *
 * READY denotes that enough state has been sent down to render the
 * terminal and reconstruct its unfinished VT Stream state.
 *
 * After READY, we send history pages (scrollback). Finally, the
 * snapshot ends with an empty FINISH marker.
 *
 * ## Snapshot Format
 *
 * This documents snapshot format 1. Version 1 is the work-in-progress
 * format that we intended to continue to break until we can promise
 * binary compatibility.
 *
 * A snapshot is one envelope followed by a sequence of records. The envelope
 * occurs once at byte zero. Every record is independently framed as a fixed
 * header followed by the number of payload bytes declared by that header.
 *
 *   +------------------+
 *   | Envelope         |
 *   +------------------+
 *   | Record 1 header  |
 *   +------------------+
 *   | Record 1 payload |
 *   +------------------+
 *   | Record 2 header  |
 *   +------------------+
 *   | Record 2 payload |
 *   +------------------+
 *   | ...              |
 *   +------------------+
 *
 * Record groups have a strict order:
 *
 *   +----------------------------------------+
 *   | TERMINAL                               |
 *   +----------------------------------------+
 *   | SCREEN * terminal.screen_count         |
 *   | PAGE * each screen.page_count          |
 *   +----------------------------------------+
 *   | CONTINUATION                           |
 *   +----------------------------------------+
 *   | READY                                  |
 *   +----------------------------------------+
 *   | HISTORY * terminal.screen_count        |
 *   | PAGE * each history.page_count         |
 *   +----------------------------------------+
 *   | FINISH                                 |
 *   +----------------------------------------+
 *
 * The SCREEN and HISTORY sequence groups may each appear in any key order.
 * Each key must occur exactly once in each group and must identify one of the
 * screens declared by TERMINAL. SCREEN contains the complete pages needed to
 * restore each active area. HISTORY contains the older complete pages for its
 * screen in newest-to-oldest order so they can be prepended as they arrive.
 * Every SCREEN has one corresponding HISTORY, even when its history page count
 * is zero. FINISH terminates the snapshot. Bytes after FINISH belong to the
 * containing transport and are not consumed by snapshot decoding.
 *
 * CONTINUATION contains the bytes required to bring the terminal's
 * VT parser/stream up to the same state, or no bytes if it should be
 * in the ground state.
 *
 * READY and FINISH are empty marker records. READY separates the renderable
 * active state and continuation from history; FINISH terminates the complete
 * snapshot. Every record, including both markers, is independently protected
 * by CRC32C. Each SCREEN declares its complete logical history extent, allowing
 * a client to size its scrollbar at READY even though older PAGE records arrive
 * afterward.
 *
 * ## Encoding
 *
 * Encode a complete snapshot into any writer:
 *
 *   std::string output;
 *   snapshot::EncodeOptions options;  // continuation defaults to ground
 *   snapshot::Error err = snapshot::encode(&output, &terminal, options);
 *
 * Encoding begins at the writer's current position, so unrelated bytes may
 * precede the snapshot. The encoder buffers only the current record payload
 * to calculate its length and CRC32C; completed records stream immediately.
 * Buffering is an encoder implementation detail, not a requirement of the
 * wire format.
 *
 * A failure may leave prior complete records, or a partial record if the
 * destination itself fails. Such a prefix has no FINISH marker and
 * cannot be restored as a complete snapshot.
 *
 * Each record type usually exposes an `encode` function that encodes
 * a complete record, such as `screen::encode`.
 *
 * ## Decoding
 *
 * `snapshot::decode` consumes exactly one snapshot through FINISH and leaves
 * any following bytes unread. This permits multiple snapshots or live protocol
 * data to share a stream without waiting for the peer to close it. Transports
 * that deliver live PTY data before history finishes must multiplex that data
 * outside this ordered snapshot record sequence.
 *
 *   snapshot::DecodeOptions options;
 *   options.max_continuation_bytes = 1024 * 1024;
 *   snapshot::Decoded decoded;
 *   snapshot::Error err = snapshot::decode(alloc, &reader, options, &decoded);
 *   ...
 *   Terminal terminal = decoded.toOwned();
 *   decoded.deinit(alloc);
 *
 * Use `snapshot::decodeExact` for a bounded file or buffer that must contain
 * only one snapshot. It preserves the stricter end-of-file check, which may
 * block when used with a live stream.
 *
 * `snapshot::Decoder` decodes the same stream incrementally so the terminal
 * becomes usable at READY, before history has arrived. Each `next` call
 * applies one history page to the by-then live terminal and returning null
 * validates FINISH:
 *
 *   snapshot::Decoder<Reader> decoder = snapshot::Decoder<Reader>::init(&reader);
 *   snapshot::Decoded decoded;
 *   err = decoder.ready(alloc, options, &decoded);
 *   Terminal terminal = decoded.toOwned();
 *
 *   // Render, replay the continuation, process input...
 *   snapshot::Decoder<Reader>::Progress progress;
 *   bool has = true;
 *   while (decoder.next(alloc, &terminal, &progress, &has) == snapshot::Error::none && has) {
 *       // Scrollback grew.
 *   }
 *
 * Wisp: Zig's `pub const x = @import(...)` re-exports become includes. The
 * component namespaces (checkpoint, continuation, envelope, grid, history,
 * hyperlink, page, record, screen, style, and terminal_record for
 * terminal.zig) already live in `wisp::terminal::snapshot`, and snapshot.hpp
 * defines encode/decode/decodeExact, Decoder, Decoded, Continuation,
 * EncodeOptions, DecodeOptions, and Error there directly, so no aliases are
 * needed. Upstream's `refAllDecls` test is a compile check; the snapshot
 * tests include this header for the same purpose.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_MAIN_HPP
#define WISP_TERMINAL_SNAPSHOT_MAIN_HPP

#include "checkpoint.hpp"
#include "continuation.hpp"
#include "envelope.hpp"
#include "grid.hpp"
#include "history.hpp"
#include "hyperlink.hpp"
#include "page.hpp"
#include "record.hpp"
#include "screen.hpp"
#include "snapshot.hpp"
#include "style.hpp"
#include "terminal.hpp"

#endif /* WISP_TERMINAL_SNAPSHOT_MAIN_HPP */
