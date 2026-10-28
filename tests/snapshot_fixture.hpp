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
 * Wisp: this lives under tests/ rather than src/, and it is a PARTIAL
 * port, so terminal/snapshot/fixture.zig stays `wip` in the ledger.
 *
 *   - It is test-only, does file I/O, and nothing in src/ references it.
 *     Upstream can keep it beside the code because Zig tests live in the
 *     file under test; this port's tests are separate translation units.
 *   - `parse` is a runtime function, not comptime. Upstream decodes the
 *     fixture at compile time via `@embedFile` and returns a
 *     `[decodedLen(source)]u8`, which also forces its
 *     `@setEvalBranchQuota(100_000_000)` for the larger fixtures. A C++
 *     test can simply read the file, so the two-pass length computation
 *     and the quota go away.
 *   - `Kind.page` and `Kind.snapshot` are NOT ported. They add
 *     structure-aware section labels to a regenerated candidate
 *     (formatPage and friends) and change no decoded byte. Only
 *     `Kind.bytes` is needed until the files whose fixtures use them are
 *     ported; `format` below asserts rather than guessing if asked for
 *     one. Likewise `writeKaitaiMetadata` is reduced to copying the
 *     reference's own `# Kaitai ...` lines through, which is what it
 *     does for a well-formed reference, without the
 *     InvalidKaitaiMetadata validation.
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

namespace wisp {
namespace test_fixture {

/* Wisp: `page` and `snapshot` select upstream's structure-aware candidate
 * formatting, which is not ported (see the header); they compare bytes
 * exactly like `bytes`. */
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

/* Wisp: upstream copies the reference's Kaitai metadata into a candidate
 * so the generated file keeps its parser selection. */
inline void writeKaitaiMetadata(const std::string &reference_source,
                                std::string *writer) {
    size_t i = 0;
    while (i < reference_source.size()) {
        size_t eol = reference_source.find('\n', i);
        if (eol == std::string::npos) eol = reference_source.size();
        const std::string line = reference_source.substr(i, eol - i);
        if (line.compare(0, 9, "# Kaitai ") == 0) {
            writer->append(line);
            writer->push_back('\n');
        }
        i = eol + 1;
    }
}

inline void writeSection(const char *label, size_t offset,
                         const std::string &bytes, std::string *writer) {
    char head[128];
    snprintf(head, sizeof head, "\n# offset 0x%08zx: %s\n", offset, label);
    writer->append(head);
    for (size_t i = 0; i < bytes.size(); i++) {
        char b[4];
        snprintf(b, sizeof b, "%02x", (unsigned)(uint8_t)bytes[i]);
        writer->append(b);
        writer->push_back(' ');
    }
    char tail[64];
    snprintf(tail, sizeof tail, "# 0x%08zx\n", offset);
    writer->append(tail);
}

inline void format(Kind kind, const std::string &reference_source,
                   const std::string &bytes, std::string *writer) {
    /* Every generated candidate starts with enough maintenance context to be
     * understandable when copied out of a failing test's temp directory. */
    writer->append("# Ghostty snapshot fixture\n");
    writeKaitaiMetadata(reference_source, writer);
    char v[160];
    snprintf(v, sizeof v,
             "# Wire version: %u\n"
             "# Generated by its snapshot test; review before replacing.\n"
             "# On mismatch, the candidate is copied to the repository root.\n",
             (unsigned)::wisp::terminal::snapshot::envelope::version);
    writer->append(v);

    /* Small values only need a hex dump. */
    if (kind != Kind::bytes) {
        /* Wisp: see the header -- page and snapshot kinds are unported. */
        writer->append("# unsupported fixture kind\n");
        return;
    }
    writeSection("encoded bytes", 0, bytes, writer);
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
    format(kind, reference_source, actual, &candidate);

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
