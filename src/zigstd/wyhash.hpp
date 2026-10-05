/* Transliterated from Zig 0.16.0 lib/std/hash/wyhash.zig
 * Copyright (c) Zig contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Ghostty hashes map keys with std.hash.Wyhash; bucket placement, and so
 * any behavior that depends on it, only matches upstream with the same
 * function. TRANSLITERATION; comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_ZIGSTD_WYHASH_HPP
#define WISP_ZIGSTD_WYHASH_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace wisp {
namespace zigstd {

struct Wyhash {
    static uint64_t secret(int i) {
        static const uint64_t s[4] = {
            0xa0761d6478bd642full,
            0xe7037ed1a0b428dbull,
            0x8ebc6af09c88c6e3ull,
            0x589965cc75374cc3ull,
        };
        return s[i];
    }

    uint64_t a;
    uint64_t b;
    uint64_t state[3];
    size_t total_len;

    uint8_t buf[48];
    size_t buf_len;

    static Wyhash init(uint64_t seed) {
        Wyhash self;
        self.a = 0;
        self.b = 0;
        self.total_len = 0;
        self.buf_len = 0;

        self.state[0] = seed ^ mix(seed ^ secret(0), secret(1));
        self.state[1] = self.state[0];
        self.state[2] = self.state[0];
        return self;
    }

    /* This is subtly different from other hash function update calls. Wyhash requires the last
     * full 48-byte block to be run through final1 if is exactly aligned to 48-bytes. */
    void update(const uint8_t *input, size_t len) {
        total_len += len;

        if (len <= 48 - buf_len) {
            memcpy(buf + buf_len, input, len);
            buf_len += len;
            return;
        }

        size_t i = 0;

        if (buf_len > 0) {
            i = 48 - buf_len;
            memcpy(buf + buf_len, input, i);
            round(buf);
            buf_len = 0;
        }

        while (i + 48 < len) {
            round(input + i);
            i += 48;
        }

        const uint8_t *remaining_bytes = input + i;
        const size_t remaining_len = len - i;
        if (remaining_len < 16 && i >= 48) {
            const size_t rem = 16 - remaining_len;
            memcpy(buf + 48 - rem, input + i - rem, rem);
        }
        memcpy(buf, remaining_bytes, remaining_len);
        buf_len = remaining_len;
    }

    uint64_t final_() {
        const uint8_t *input = buf;
        size_t input_len = buf_len;
        Wyhash newSelf = shallowCopy(); /* ensure idempotency */

        if (total_len <= 16) {
            newSelf.smallKey(input, input_len);
        } else {
            size_t offset = 0;
            uint8_t scratch[16];
            if (buf_len < 16) {
                const size_t rem = 16 - buf_len;
                memcpy(scratch, buf + 48 - rem, rem);
                memcpy(scratch + rem, buf, buf_len);

                /* Same as input but with additional bytes preceding start in case of a short buffer */
                input = scratch;
                input_len = 16;
                offset = rem;
            }

            newSelf.final0();
            newSelf.final1(input, input_len, offset);
        }

        return newSelf.final2();
    }

    /* Copies the core wyhash state but not any internal buffers. */
    Wyhash shallowCopy() const {
        Wyhash c;
        c.a = a;
        c.b = b;
        c.state[0] = state[0];
        c.state[1] = state[1];
        c.state[2] = state[2];
        c.total_len = total_len;
        c.buf_len = 0;
        return c;
    }

    void smallKey(const uint8_t *input, size_t len) {
        /* assert(input.len <= 16) */
        if (len >= 4) {
            const size_t end = len - 4;
            const size_t quarter = (len >> 3) << 2;
            a = (read(4, input) << 32) | read(4, input + quarter);
            b = (read(4, input + end) << 32) | read(4, input + end - quarter);
        } else if (len > 0) {
            a = ((uint64_t)input[0] << 16) | ((uint64_t)input[len >> 1] << 8) | input[len - 1];
            b = 0;
        } else {
            a = 0;
            b = 0;
        }
    }

    void round(const uint8_t *input) {
        for (int i = 0; i < 3; i++) {
            const uint64_t ra = read(8, input + 8 * (2 * i));
            const uint64_t rb = read(8, input + 8 * (2 * i + 1));
            state[i] = mix(ra ^ secret(i + 1), rb ^ state[i]);
        }
    }

    static uint64_t read(size_t bytes, const uint8_t *data) {
        /* std.mem.readInt(T, data[0..bytes], .little) */
        uint64_t v = 0;
        for (size_t k = 0; k < bytes; k++) v |= (uint64_t)data[k] << (8 * k);
        return v;
    }

    static void mum(uint64_t *pa, uint64_t *pb) {
#ifdef _MSC_VER
        uint64_t hi;
        const uint64_t lo = _umul128(*pa, *pb, &hi);
        *pa = lo;
        *pb = hi;
#else
        const unsigned __int128 x = (unsigned __int128)*pa * *pb;
        *pa = (uint64_t)x;
        *pb = (uint64_t)(x >> 64);
#endif
    }

    static uint64_t mix(uint64_t a_, uint64_t b_) {
        uint64_t x = a_;
        uint64_t y = b_;
        mum(&x, &y);
        return x ^ y;
    }

    void final0() { state[0] ^= state[1] ^ state[2]; }

    /* input_lb must be at least 16-bytes long (in shorter key cases the smallKey function will be
     * used instead). We use an index into a slice to for comptime processing as opposed to if we
     * used pointers. */
    void final1(const uint8_t *input_lb, size_t input_lb_len, size_t start_pos) {
        /* assert(input_lb.len >= 16); assert(input_lb.len - start_pos <= 48) */
        const uint8_t *input = input_lb + start_pos;
        const size_t input_len = input_lb_len - start_pos;

        size_t i = 0;
        while (i + 16 < input_len) {
            state[0] = mix(read(8, input + i) ^ secret(1), read(8, input + i + 8) ^ state[0]);
            i += 16;
        }

        a = read(8, input_lb + input_lb_len - 16);
        b = read(8, input_lb + input_lb_len - 8);
    }

    uint64_t final2() {
        a ^= secret(1);
        b ^= state[0];
        mum(&a, &b);
        return mix(a ^ secret(0) ^ total_len, b ^ secret(1));
    }

    static uint64_t hash(uint64_t seed, const void *data, size_t len) {
        const uint8_t *input = (const uint8_t *)data;
        Wyhash self = init(seed);

        if (len <= 16) {
            self.smallKey(input, len);
        } else {
            size_t i = 0;
            if (len >= 48) {
                while (i + 48 < len) {
                    self.round(input + i);
                    i += 48;
                }
                self.final0();
            }
            self.final1(input, len, i);
        }

        self.total_len = len;
        return self.final2();
    }
};

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_WYHASH_HPP */
