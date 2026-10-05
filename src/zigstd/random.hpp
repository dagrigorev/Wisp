/* Transliterated from Zig 0.16.0 lib/std/Random.zig, Random/Xoshiro256.zig
 * and Random/SplitMix64.zig
 * Copyright (c) Zig contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Ghostty's tests seed std.Random.DefaultPrng (Xoshiro256++) and draw with
 * int / uintLessThan / intRangeLessThan / shuffle; porting those tests with
 * the same inputs needs the same generator. Only what those tests use is
 * here. TRANSLITERATION; comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_ZIGSTD_RANDOM_HPP
#define WISP_ZIGSTD_RANDOM_HPP

#include <stddef.h>
#include <stdint.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace wisp {
namespace zigstd {

/* Generator to extend 64-bit seed values into longer sequences.
 *
 * The number of cycles is thus limited to 64-bits regardless of the engine, but this
 * is still plenty for practical purposes. */
struct SplitMix64 {
    uint64_t s;

    static SplitMix64 init(uint64_t seed) {
        SplitMix64 g;
        g.s = seed;
        return g;
    }

    uint64_t next() {
        s += 0x9e3779b97f4a7c15ull;

        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
};

inline uint64_t rotl64(uint64_t x, unsigned r) { return (x << r) | (x >> ((64 - r) & 63)); }

/* Xoshiro256++ - http://xoroshiro.di.unimi.it/
 *
 * PRNG */
struct Xoshiro256 {
    uint64_t s[4];

    static Xoshiro256 init(uint64_t init_s) {
        Xoshiro256 x;
        x.seed(init_s);
        return x;
    }

    uint64_t next() {
        const uint64_t r = rotl64(s[0] + s[3], 23) + s[0];

        const uint64_t t = s[1] << 17;

        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];

        s[2] ^= t;

        s[3] = rotl64(s[3], 45);

        return r;
    }

    void seed(uint64_t init_s) {
        /* Xoshiro requires 256-bits of seed. */
        SplitMix64 gen = SplitMix64::init(init_s);

        s[0] = gen.next();
        s[1] = gen.next();
        s[2] = gen.next();
        s[3] = gen.next();
    }

    void fill(uint8_t *buf, size_t len) {
        size_t i = 0;
        const size_t aligned_len = len - (len & 7);

        /* Complete 8 byte segments. */
        while (i < aligned_len) {
            uint64_t n = next();
            for (size_t j = 0; j < 8; j++) {
                buf[i + j] = (uint8_t)n;
                n >>= 8;
            }
            i += 8;
        }

        /* Remaining. (cuts the stream) */
        if (i != len) {
            uint64_t n = next();
            while (i < len) {
                buf[i] = (uint8_t)n;
                n >>= 8;
                i += 1;
            }
        }
    }
};

typedef Xoshiro256 DefaultPrng;

/* Wisp: std.Random over a Xoshiro256 — the interface's fillFn is the
 * generator's fill. Unsigned T only; widths of 8, 16, 32 and 64 bits. */
struct Random {
    Xoshiro256 *prng;

    explicit Random(Xoshiro256 *p) : prng(p) {}

    void bytes(uint8_t *buf, size_t len) { prng->fill(buf, len); }

    template <typename T>
    T int_() {
        uint8_t rand_bytes[sizeof(T)];
        bytes(rand_bytes, sizeof(T));

        /* use LE instead of native endian for better portability maybe? */
        uint64_t v = 0;
        for (size_t k = 0; k < sizeof(T); k++) v |= (uint64_t)rand_bytes[k] << (8 * k);
        return (T)v;
    }

    /* Wisp: math.mulWide(T, x, less_than), split into low and high halves
     * of the 2*bits product. */
    template <typename T>
    static void mulWide(T x, T y, T *lo, T *hi) {
        if (sizeof(T) < 8) {
            const uint64_t m = (uint64_t)x * (uint64_t)y;
            *lo = (T)m;
            *hi = (T)(m >> ((8 * sizeof(T)) & 63));
        } else {
#ifdef _MSC_VER
            uint64_t h;
            const uint64_t l = _umul128((uint64_t)x, (uint64_t)y, &h);
            *lo = (T)l;
            *hi = (T)h;
#else
            const unsigned __int128 m = (unsigned __int128)x * y;
            *lo = (T)m;
            *hi = (T)(m >> 64);
#endif
        }
    }

    /* Returns an evenly distributed random unsigned integer `0 <= i < less_than`.
     * This function assumes that the underlying `fillFn` produces evenly distributed values.
     * Within this assumption, the runtime of this function is exponentially distributed.
     * If `fillFn` were backed by a true random generator,
     * the runtime of this function would technically be unbounded.
     * However, if `fillFn` is backed by any evenly distributed pseudo random number generator,
     * this function is guaranteed to return.
     * If you need deterministic runtime bounds, use `uintLessThanBiased`. */
    template <typename T>
    T uintLessThan(T less_than) {
        /* assert(0 < less_than) */

        /* adapted from:
         *   http://www.pcg-random.org/posts/bounded-rands.html
         *   "Lemire's (with an extra tweak from me)" */
        T x = int_<T>();
        T l, h;
        mulWide<T>(x, less_than, &l, &h);
        if (l < less_than) {
            T t = (T)(0 - less_than);

            if (t >= less_than) {
                t -= less_than;
                if (t >= less_than) {
                    t %= less_than;
                }
            }
            while (l < t) {
                x = int_<T>();
                mulWide<T>(x, less_than, &l, &h);
            }
        }
        return h;
    }

    /* Returns an evenly distributed random unsigned integer `0 <= i <= at_most`. */
    template <typename T>
    T uintAtMost(T at_most) {
        if (at_most == (T)~(T)0) return int_<T>();
        return uintLessThan<T>((T)(at_most + 1));
    }

    template <typename T>
    T intRangeAtMost(T at_least, T at_most) {
        /* assert(at_least <= at_most) */
        return (T)(at_least + uintAtMost<T>((T)(at_most - at_least)));
    }

    /* Wisp: int(u1) reads one byte and truncates to its low bit. */
    bool boolean() { return (int_<uint8_t>() & 1) != 0; }

    template <typename T>
    T intRangeLessThan(T at_least, T less_than) {
        /* assert(at_least < less_than)
         * The signed implementation would work fine, but we can use stricter arithmetic operators here. */
        return (T)(at_least + uintLessThan<T>((T)(less_than - at_least)));
    }

    /* Wisp: shuffle(T, buf) is shuffleWithIndex(T, buf, usize). */
    template <typename E>
    void shuffle(E *buf, size_t len) {
        if (len < 2) return;

        /* `i <= j < max <= maxInt(MinInt)` */
        const uint64_t max = len;
        uint64_t i = 0;
        while (i < max - 1) {
            const uint64_t j = intRangeLessThan<uint64_t>(i, max);
            const E tmp = buf[i];
            buf[i] = buf[j];
            buf[j] = tmp;
            i += 1;
        }
    }
};

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_RANDOM_HPP */
