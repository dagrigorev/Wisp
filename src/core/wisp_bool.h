#pragma once
#ifndef WISP_BOOL_H
#define WISP_BOOL_H

/*
 * wisp_bool.h — portable C/C++ bool compatibility.
 *
 * MSVC's <stdbool.h> can resolve to the C++ STL compatibility header in some
 * toolchain configurations, which then includes yvals_core.h and fails when a
 * .c translation unit is compiled as C:
 *   STL1003: Unexpected compiler, expected C++ compiler.
 *
 * Keep C sources independent from MSVC's STL headers.  C++ sources use native
 * bool/true/false.  Non-MSVC C compilers use the standard C99 header.
 */

#ifdef __cplusplus
/* native C++ bool */
#else
#  if defined(_MSC_VER) && !defined(__clang__)
     typedef unsigned char bool;
#    ifndef true
#      define true 1
#    endif
#    ifndef false
#      define false 0
#    endif
#  else
#    include <stdbool.h>
#  endif
#endif

#endif /* WISP_BOOL_H */
