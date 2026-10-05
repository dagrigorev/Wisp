/* Transliterated from Ghostty src/tripwire.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A library for injecting failures into Zig code for the express
 * purpose of testing error handling paths.
 *
 * Improper `errdefer` is one of the highest sources of bugs in Zig code.
 * Many `errdefer` points are hard to exercise in unit tests and rare
 * to encounter in production, so they often hide bugs. Worse, error
 * scenarios are most likely to put your code in an unexpected state
 * that can result in future assertion failures or memory safety issues.
 *
 * This module aims to solve this problem by providing a way to inject
 * errors at specific points in your code during unit tests, allowing you
 * to test every possible error path.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: `tripwire.module(P, E)` is Module<P, E, point_count>. P is an enum
 * class of fail points numbered from zero; E is the error enum of the
 * function, whose value `none` means success, so `try tw.check(.x)` is
 * `if (E e = tw::check(P::x); e != E::none) return e` (spelled in C++14).
 * Enabled when WISP_IS_TEST is set, as upstream's builtin.is_test.
 */

#pragma once
#ifndef WISP_VT_TRIPWIRE_HPP
#define WISP_VT_TRIPWIRE_HPP

#include <stddef.h>

namespace wisp {
namespace tripwire {

enum class ResetMode { reset, retain };

/* A tripwire module that can be used to inject failures at specific points.
 *
 * Outside of unit tests, this module is free and completely optimized away.
 * It takes up zero binary or runtime space and all function calls are
 * optimized out. */
template <typename P, typename E, size_t point_count>
struct Module {
    /* The points this module can fail at. */
    typedef P FailPoint;

    /* The error set used for failures at the failure points. */
    typedef E Error;

    /* Whether our module is enabled or not. In the future we may
     * want to make this a comptime parameter to the module. */
#if defined(WISP_IS_TEST) && WISP_IS_TEST
    static const bool enabled = true;
#else
    static const bool enabled = false;
#endif

    struct Tripwire {
        bool set; /* Wisp: EnumMap presence */

        /* Error to return when tripped */
        Error err;

        /* The amount of times this tripwire has been reached. This
         * is NOT the number of times it has tripped, since we may
         * have mins for that. */
        size_t reached; /* = 0 */

        /* The minimum number of times this must be reached before
         * tripping. After this point, it trips every time. This is
         * a "before" check so if this is "1" then it'll trip the
         * second time it's reached. */
        size_t min; /* = 0 */

        /* True if this has been tripped at least once. */
        bool tripped; /* = false */
    };

    /* The configured tripwires for this module. */
    static Tripwire *tripwires() {
        static Tripwire t[point_count];
        return t;
    }

    /* Check for a failure at the given failure point. These should
     * be placed directly before the `try` operation that may fail.
     * Wisp: returns Error::none when not tripped. */
    static Error check(FailPoint point) {
        if (!enabled) return Error::none;
        Tripwire &tw = tripwires()[(size_t)point];
        if (!tw.set) return Error::none;
        tw.reached += 1;
        if (tw.reached <= tw.min) return Error::none;
        tw.tripped = true;
        return tw.err;
    }

    /* Mark a failure point to always trip with the given error. */
    static void errorAlways(FailPoint point, Error err) { errorAfter(point, err, 0); }

    /* Mark a failure point to trip with the given error after
     * the failure point is reached at least `min` times. A value of
     * zero is equivalent to `errorAlways`. */
    static void errorAfter(FailPoint point, Error err, size_t min) {
        Tripwire &tw = tripwires()[(size_t)point];
        tw.set = true;
        tw.err = err;
        tw.reached = 0;
        tw.min = min;
        tw.tripped = false;
    }

    /* Ends the tripwire session. This will raise an error if there
     * were untripped error expectations. The reset mode specifies
     * whether expectations are reset too. Expectations are always reset,
     * even if this returns an error.
     * Wisp: false is error.UntrippedError. */
    static bool end(ResetMode reset_mode) {
        bool untripped = false;
        for (size_t i = 0; i < point_count; i++) {
            const Tripwire &tw = tripwires()[i];
            if (tw.set && !tw.tripped) {
                /* log.warn("untripped point={s}") */
                untripped = true;
            }
        }

        if (reset_mode == ResetMode::reset) reset();

        return !untripped;
    }

    /* Unset all the tripwires. You should usually call `end` instead. */
    static void reset() {
        for (size_t i = 0; i < point_count; i++) {
            Tripwire &tw = tripwires()[i];
            tw.set = false;
            tw.reached = 0;
            tw.min = 0;
            tw.tripped = false;
        }
    }
};

} /* namespace tripwire */
} /* namespace wisp */

#endif /* WISP_VT_TRIPWIRE_HPP */
