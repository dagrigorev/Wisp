/* Wisp: the platform CSPRNG behind terminal::sys::randomSecure. Ghostty
 * gets this from std.Io; Wisp has no Io, so the platform call lives here
 * and sys.hpp stays free of platform headers.
 *
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#include "sys.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <stdio.h>
#endif

namespace wisp {
namespace terminal {
namespace sys {

RandomSecureError platformRandomSecure(uint8_t *buffer, size_t len) {
#ifdef _WIN32
    const NTSTATUS status =
        BCryptGenRandom(nullptr, (PUCHAR)buffer, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) return RandomSecureError::EntropyUnavailable;
    return RandomSecureError::none;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == nullptr) return RandomSecureError::EntropyUnavailable;
    const size_t n = fread(buffer, 1, len, f);
    fclose(f);
    if (n != len) return RandomSecureError::EntropyUnavailable;
    return RandomSecureError::none;
#endif
}

} /* namespace sys */
} /* namespace terminal */
} /* namespace wisp */
