/* Transliterated from Ghostty src/terminal/snapshot/fixture.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Golden-fixture support for the snapshot wire format: decode an
 * annotated hex reference, and compare encoder output against it.
 *
 * Wisp: this lives under tests/ rather than src/.
 *
 *   - It is test-only, does file I/O, and nothing in src/ references it.
 *     Upstream can keep it beside the code because Zig tests live in the
 *     file under test; this port's tests are separate translation units.
 *   - `parse` is a runtime function, not comptime. Upstream decodes the
 *     fixture at compile time via `@embedFile` and returns a
 *     `[decodedLen(source)]u8`, which also forces its
 *     `@setEvalBranchQuota(100_000_000)` for the larger fixtures. A C++
 *     test can simply read the file, so the two-pass length computation
 *     and the quota go away; syntax errors become a false return instead
 *     of @compileError.
 *   - Upstream formats the candidate into a temporary directory and copies
 *     it to the repository root on mismatch. Here the candidate is always
 *     formatted in memory and written to the working directory only on
 *     mismatch, which leaves the same file in the same place.
 *   - An `InvalidKaitaiMetadata` reference makes `expectEqual` fail, as
 *     upstream's `try format(...)` does.
 *
 * Upstream's generation behaviour IS kept, because it is deliberate: a
 * candidate is materialized on every run, matching or not, so the
 * generation path is always exercised; a matching candidate is discarded,
 * and a mismatching one is left beside the test binary for review.
 */
#pragma once
#ifndef WISP_TESTS_SNAPSHOT_FIXTURE_HPP
#define WISP_TESTS_SNAPSHOT_FIXTURE_HPP

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../src/terminal/snapshot/envelope.hpp"
#include "../src/terminal/snapshot/record.hpp"

namespace wisp {
namespace test_fixture {

/* Selects how a generated candidate is annotated. */
enum class Kind { bytes, page, snapshot };

/* Decode an annotated hexadecimal fixture.
 *
 * Whitespace is ignored. `#` begins a comment that continues through the end
 * of the line. Every other token must be exactly two hexadecimal digits. */
inline bool hexNibble(char c, unsigned *out) {
    if (c >= '0' && c <= '9') { *out = (unsigned)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned)(c - 'A' + 10); return true; }
    return false;
}

inline void skipIgnored(const char *src, size_t len, size_t *i) {
    for (;;) {
        while (*i < len && (src[*i] == ' ' || src[*i] == '\t' ||
                            src[*i] == '\r' || src[*i] == '\n')) {
            (*i)++;
        }
        if (*i < len && src[*i] == '#') {
            while (*i < len && src[*i] != '\n') (*i)++;
            continue;
        }
        return;
    }
}

inline bool parse(const char *src, size_t len, std::string *out) {
    out->clear();
    size_t i = 0;
    while (i < len) {
        skipIgnored(src, len, &i);
        if (i == len) break;

        /* Every data token is one complete byte. Parse it directly rather
         * than accepting variable-width integers or other hex syntax. */
        if (i + 1 >= len) return false; /* ends with one hex digit */
        unsigned hi = 0, lo = 0;
        if (!hexNibble(src[i], &hi)) return false;
        if (!hexNibble(src[i + 1], &lo)) return false;
        out->push_back((char)(uint8_t)((hi << 4) | lo));
        i += 2;
    }
    return true;
}

inline bool readFile(const char *path, std::string *out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    out->clear();
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

/* Lowercase, sixteen-byte lines provide stable textual diffs. The
 * trailing offset is a comment and is ignored by parse. */
inline void writeHex(const std::string &bytes, size_t start, size_t end,
                     std::string *writer) {
    static const char digits[] = "0123456789abcdef";
    size_t offset = start;
    while (offset < end) {
        const size_t line_end = end < offset + 16 ? end : offset + 16;
        for (size_t index = 0; offset + index < line_end; index++) {
            const uint8_t byte = (uint8_t)bytes[offset + index];
            if (index != 0) writer->push_back(' ');
            writer->push_back(digits[byte >> 4]);
            writer->push_back(digits[byte & 0x0f]);
        }
        char tail[32];
        snprintf(tail, sizeof tail, " # 0x%08zx\n", offset);
        writer->append(tail);
        offset = line_end;
    }
}

/* Sections begin on a fresh line and carry their absolute wire offset. */
inline void writeSection(const char *name, const std::string &bytes,
                         size_t start, size_t end, std::string *writer) {
    char head[160];
    snprintf(head, sizeof head, "\n# offset 0x%08zx: %s\n", start, name);
    writer->append(head);
    writeHex(bytes, start, end, writer);
}

/* Wisp: false is error.InvalidKaitaiMetadata. */
inline bool writeKaitaiMetadata(const std::string &reference_source,
                                std::string *writer) {
    static const char *const prefixes[] = {
        "# Kaitai type:",
        "# Kaitai params:",
        "# Kaitai offset:",
    };
    bool found[3] = {false, false, false};

    size_t i = 0;
    while (i <= reference_source.size()) {
        size_t eol = reference_source.find('\n', i);
        if (eol == std::string::npos) eol = reference_source.size();
        std::string line = reference_source.substr(i, eol - i);
        /* std.mem.trim(u8, untrimmed, " \r") */
        const size_t first = line.find_first_not_of(" \r");
        const size_t last = line.find_last_not_of(" \r");
        line = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
        for (size_t p = 0; p < 3; p++) {
            if (line.compare(0, strlen(prefixes[p]), prefixes[p]) != 0) continue;
            if (found[p]) return false;
            found[p] = true;
            writer->append(line);
            writer->push_back('\n');
            break;
        }
        i = eol + 1;
    }

    for (size_t p = 0; p < 3; p++) {
        if (!found[p]) return false;
    }
    return true;
}

inline void formatPage(const std::string &bytes, std::string *writer) {
    /* PAGE version 1 has a twenty-byte fixed header. The remainder is
     * self-describing through the counts and dimensions in that header. */
    const size_t header_end = bytes.size() < 20 ? bytes.size() : 20;
    writeSection("PAGE header", bytes, 0, header_end, writer);
    if (header_end < bytes.size()) {
        writeSection("style and hyperlink tables, rows, and cells", bytes,
                     header_end, bytes.size(), writer);
    }
}

/* Wisp: std.enums.fromInt(record.Tag, raw) plus @tagName. */
inline const char *recordTagName(uint16_t raw) {
    typedef ::wisp::terminal::snapshot::record::Tag Tag;
    switch ((Tag)raw) {
        case Tag::terminal: return "terminal";
        case Tag::screen: return "screen";
        case Tag::page: return "page";
        case Tag::history: return "history";
        case Tag::ready: return "ready";
        case Tag::finish: return "finish";
        case Tag::continuation: return "continuation";
    }
    return nullptr;
}

inline void formatSnapshot(const std::string &bytes, std::string *writer) {
    /* A complete snapshot begins with its unframed envelope. Clamp this first
     * section so a partially generated candidate can still be inspected. */
    const size_t envelope_len = ::wisp::terminal::snapshot::envelope::encoded_len;
    const size_t envelope_end = bytes.size() < envelope_len ? bytes.size() : envelope_len;
    writeSection("snapshot envelope", bytes, 0, envelope_end, writer);

    /* Walk record framing only far enough to annotate boundaries. The emitted
     * hex remains authoritative; this formatter never repairs candidate data. */
    const size_t header_len = ::wisp::terminal::snapshot::record::Header::len;
    size_t offset = envelope_end;
    while (offset < bytes.size()) {
        /* A partial header cannot provide a safe payload boundary, so preserve
         * the remaining bytes as one explicitly incomplete section. */
        if (bytes.size() - offset < header_len) {
            writeSection("trailing partial record", bytes, offset, bytes.size(), writer);
            return;
        }

        /* Tag and payload length are the first six bytes of every record
         * header. CRC validation belongs to the codec test, not this formatter. */
        const uint8_t *b = (const uint8_t *)bytes.data() + offset;
        const uint16_t tag_raw = (uint16_t)(b[0] | (b[1] << 8));
        const uint32_t payload_len = (uint32_t)b[2] | ((uint32_t)b[3] << 8) |
                                     ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);

        /* Clamp the section to the available candidate bytes. This makes a
         * truncated payload reviewable instead of indexing beyond the slice. */
        const size_t want = offset + header_len + (size_t)payload_len;
        const size_t record_end = bytes.size() < want ? bytes.size() : want;

        /* Known tags make the annotated diff readable. Preserve unknown tags
         * numerically so formatter output is still useful for encoder bugs. */
        char head[160];
        if (const char *name = recordTagName(tag_raw)) {
            snprintf(head, sizeof head, "\n# offset 0x%08zx: %s record, payload %u bytes\n",
                     offset, name, (unsigned)payload_len);
        } else {
            snprintf(head, sizeof head, "\n# offset 0x%08zx: unknown record %u, payload %u bytes\n",
                     offset, (unsigned)tag_raw, (unsigned)payload_len);
        }
        writer->append(head);
        writeHex(bytes, offset, record_end, writer);
        offset = record_end;
    }
}

/* Wisp: false is error.InvalidKaitaiMetadata. */
inline bool format(Kind kind, const std::string &reference_source,
                   const std::string &bytes, std::string *writer) {
    /* Every generated candidate starts with enough maintenance context to be
     * understandable when copied out of a failing test's temp directory. */
    writer->append("# Ghostty snapshot fixture\n");
    if (!writeKaitaiMetadata(reference_source, writer)) return false;
    char v[200];
    snprintf(v, sizeof v,
             "# Wire version: %u\n"
             "# Generated by its snapshot test; review before replacing.\n"
             "# On mismatch, the candidate is copied to the repository root.\n",
             (unsigned)::wisp::terminal::snapshot::envelope::version);
    writer->append(v);

    /* Small values only need a hex dump. Larger compound formats get
     * structure-aware section labels without changing their decoded bytes. */
    switch (kind) {
        case Kind::bytes: writeSection("encoded bytes", bytes, 0, bytes.size(), writer); break;
        case Kind::page: formatPage(bytes, writer); break;
        case Kind::snapshot: formatSnapshot(bytes, writer); break;
    }
    return true;
}

/* Returns true when the bytes match. On mismatch, writes the candidate
 * beside the test binary and prints the first differing offset. */
inline bool expectEqual(Kind kind, const char *reference_path,
                        const char *candidate_name,
                        const std::string &expected,
                        const std::string &actual) {
    std::string reference_source;
    if (!readFile(reference_path, &reference_source)) {
        fprintf(stderr, "  FAIL  cannot read fixture reference %s\n",
                reference_path);
        return false;
    }

    /* Always materialize a candidate, even when it will match. This keeps the
     * generation path exercised on every test run. */
    std::string candidate;
    if (!format(kind, reference_source, actual, &candidate)) {
        fprintf(stderr, "  FAIL  InvalidKaitaiMetadata in %s\n", reference_path);
        return false;
    }

    if (expected == actual) return true;

    FILE *f = fopen(candidate_name, "wb");
    if (f != NULL) {
        fwrite(candidate.data(), 1, candidate.size(), f);
        fclose(f);
    }

    /* Report the first useful comparison point. When one byte sequence is a
     * prefix of the other, this is the shared length where they diverge. */
    const size_t shared_len =
        expected.size() < actual.size() ? expected.size() : actual.size();
    size_t difference = shared_len;
    for (size_t i = 0; i < shared_len; i++) {
        if (expected[i] != actual[i]) { difference = i; break; }
    }
    fprintf(stderr,
            "  FAIL  snapshot fixture differs reference=%s candidate=%s "
            "offset=0x%zx expected_len=%zu actual_len=%zu\n",
            reference_path, candidate_name, difference, expected.size(),
            actual.size());
    return false;
}

} /* namespace test_fixture */
} /* namespace wisp */

#endif /* WISP_TESTS_SNAPSHOT_FIXTURE_HPP */
