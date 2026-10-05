/* Transliterated from Ghostty src/terminal/kitty/clipboard_grants.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Kitty clipboard protocol (OSC 5522) session password grants:
 * requests carrying a granted password skip the permission prompt, and
 * paste events mint one-time passwords.
 *
 * Wisp, differences in shape rather than behavior:
 *   - std.ArrayListUnmanaged(Entry) is a fixed array of max_entries plus a
 *     length. The list never grows past max_entries upstream either, so
 *     the only allocation left is the owned password copy.
 *   - `generateOtp` returns the error through its return value and writes
 *     the password through an out parameter.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_CLIPBOARD_GRANTS_HPP
#define WISP_TERMINAL_KITTY_CLIPBOARD_GRANTS_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../zigstd/allocator.hpp"
#include "../sys.hpp"
#include "clipboard_command.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace clipboard {

/* Session password grants, used to skip permission prompts for
 * requests carrying a known pw. Callers can choose to scope these
 * however they want, e.g. Kitty does it per window and the spec
 * doesn't demand anything. */
struct Grants {
    static const size_t max_entries = 32;

    enum class Direction : uint8_t { read, write };

    struct Entry {
        /* Owned by the allocator passed to grant. */
        uint8_t *pw;
        size_t pw_len;
        bool read;     /* = false */
        bool write;    /* = false */
        bool one_time; /* = false */

        Entry() : pw(nullptr), pw_len(0), read(false), write(false), one_time(false) {}
    };

    Entry entries[max_entries];
    size_t entries_len; /* = .empty */

    Grants() : entries_len(0) {}

    void deinit(zigstd::Allocator alloc) {
        for (size_t i = 0; i < entries_len; i++) alloc.freeT<uint8_t>(entries[i].pw, entries[i].pw_len);
        entries_len = 0;
    }

    /* Record a grant for pw. An existing grant for the same password
     * gains the new direction.
     * Wisp: returns false on Allocator.Error. */
    bool grant(zigstd::Allocator alloc, const uint8_t *pw, size_t pw_len, Direction dir, bool one_time) {
        if (pw_len == 0 || pw_len > max_pw_len) return true;

        Entry *entry;
        {
            size_t idx;
            if (findIndex(pw, pw_len, &idx)) {
                entry = &entries[idx];
                entry->one_time = entry->one_time && one_time;
            } else {
                /* Evict the oldest grant once full. */
                if (entries_len >= max_entries) {
                    Entry oldest = entries[0];
                    for (size_t i = 1; i < entries_len; i++) entries[i - 1] = entries[i];
                    entries_len -= 1;
                    alloc.freeT<uint8_t>(oldest.pw, oldest.pw_len);
                }

                uint8_t *owned = alloc.allocT<uint8_t>(pw_len);
                if (owned == nullptr) return false;
                memcpy(owned, pw, pw_len);
                Entry e;
                e.pw = owned;
                e.pw_len = pw_len;
                e.one_time = one_time;
                entries[entries_len] = e;
                entries_len += 1;
                entry = &entries[entries_len - 1];
            }
        }

        switch (dir) {
        case Direction::read: entry->read = true; break;
        case Direction::write: entry->write = true; break;
        }
        return true;
    }

    /* Check whether pw grants the given direction. A one-time grant is
     * consumed by this check even when the direction doesn't match,
     * matching kitty's pop-on-check behavior. */
    bool use(zigstd::Allocator alloc, const uint8_t *pw, size_t pw_len, Direction dir) {
        if (pw_len == 0) return false;
        size_t idx;
        if (!findIndex(pw, pw_len, &idx)) return false;
        Entry *entry = &entries[idx];
        bool allowed = false;
        switch (dir) {
        case Direction::read: allowed = entry->read; break;
        case Direction::write: allowed = entry->write; break;
        }
        if (entry->one_time) {
            /* Wisp: ArrayList.swapRemove. */
            Entry removed = entries[idx];
            entries[idx] = entries[entries_len - 1];
            entries_len -= 1;
            alloc.freeT<uint8_t>(removed.pw, removed.pw_len);
        }
        return allowed;
    }

    bool findIndex(const uint8_t *pw, size_t pw_len, size_t *out) const {
        for (size_t idx = 0; idx < entries_len; idx++) {
            const Entry *entry = &entries[idx];
            if (entry->pw_len == pw_len && memcmp(entry->pw, pw, pw_len) == 0) {
                *out = idx;
                return true;
            }
        }
        return false;
    }
};

/* The length of a one-time password generated for paste events. */
static const size_t otp_len = 22;

/* The one-time password alphabet. This matches kitty (alphanumeric
 * without easily-confused characters), but the spec doesn't demand
 * this. */
static const char otp_alphabet[] = "23456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";
static const size_t otp_alphabet_len = sizeof(otp_alphabet) - 1;

/* Generate a one-time password for a paste event.
 *
 * The password is a secret: a program that learns it can read the
 * clipboard without a prompt. Entropy comes from `sys.random_secure`
 * if set, otherwise from the platform CSPRNG; see `sys.randomSecure`. */
inline sys::RandomSecureError generateOtp(uint8_t *result /* [otp_len] */) {
    size_t len = 0;
    while (len < otp_len) {
        uint8_t raw[2 * otp_len];
        const sys::RandomSecureError err = sys::randomSecure(raw, sizeof raw);
        if (err != sys::RandomSecureError::none) return err;
        const unsigned limit = (unsigned)((UINT8_MAX + 1) / otp_alphabet_len * otp_alphabet_len);
        for (size_t i = 0; i < sizeof raw; i++) {
            const uint8_t byte = raw[i];
            if (byte >= limit) continue;
            result[len] = (uint8_t)otp_alphabet[byte % otp_alphabet_len];
            len += 1;
            if (len == otp_len) break;
        }
    }

    return sys::RandomSecureError::none;
}

} /* namespace clipboard */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_CLIPBOARD_GRANTS_HPP */
