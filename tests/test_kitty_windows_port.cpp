/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/windows.zig and src/terminal/kitty/graphics_animation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <stdio.h>
#include <string.h>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_animation.hpp"
#include "../terminal/kitty/windows.hpp"
#include "../zigstd/allocator.hpp"

using namespace wisp;

namespace kwin = wisp::terminal::kitty::windows;
namespace kanim = wisp::terminal::kitty::graphics_animation;

typedef kwin::Error Error;
typedef kanim::Animation Animation;

/* Wisp: upstream passes `[]const u8` slices; these call sites are all
 * literals without embedded NULs, so the length comes from strlen.
 * The one place that matters is "" (length 0). */
static Error checkPath(const char *path) { return kwin::checkPath(path, strlen(path)); }
static Error checkCanonicalPath(const char *path) {
    return kwin::checkCanonicalPath(path, strlen(path));
}
static bool isDriveAbsolute(const char *path) {
    return kwin::isDriveAbsolute(path, strlen(path));
}

/* Wisp: `testing.expectError(err, checkPath(path))` inside a loop, with
 * the offending path printed because ASSERT_* only reports the line. */
static bool expectErrFor(Error want, Error got, const char *path) {
    if (want == got) return true;
    fprintf(stderr, "    path \"%s\": expected error %d, got %d\n", path, (int)want, (int)got);
    return false;
}

TEST(kitty_windows, checkPath_rejects_UNC_paths) {
    static const char *const unc[] = {
        "\\\\server\\share\\tty-graphics-protocol-image.data",
        "//server/share/tty-graphics-protocol-image.data",
        "\\/server/share/image.data",
        "/\\server\\share\\image.data",
        "\\\\.x\\share\\image.data",
        "\\\\",
    };
    for (size_t i = 0; i < sizeof(unc) / sizeof(unc[0]); i += 1) {
        ASSERT_TRUE(expectErrFor(Error::UncPath, checkPath(unc[i]), unc[i]));
    }
}

TEST(kitty_windows, checkPath_rejects_device_and_NT_namespaces) {
    static const char *const device[] = {
        "\\\\?\\UNC\\server\\share\\image.data",
        "\\\\?\\C:\\Windows\\Temp\\image.data",
        "//?/C:/Windows/Temp/image.data",
        "\\\\.\\C:\\Windows\\Temp\\image.data",
        "\\\\.\\pipe\\image",
        "\\\\.\\PhysicalDrive0",
        "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\image.data",
        "//?/GLOBALROOT/Device/HarddiskVolume1/image.data",
        "\\??\\C:\\Windows\\Temp\\image.data",
        "\\??\\Device\\HarddiskVolume1\\image.data",
        "\\??\\UNC\\server\\share\\image.data",
        "/??/C:/image.data",
    };
    for (size_t i = 0; i < sizeof(device) / sizeof(device[0]); i += 1) {
        ASSERT_TRUE(expectErrFor(Error::DevicePath, checkPath(device[i]), device[i]));
    }
}

TEST(kitty_windows, checkPath_accepts_plain_local_paths) {
    static const char *const accepted[] = {
        "C:\\Windows\\Temp\\tty-graphics-protocol-image.data",
        "C:/Windows/Temp/tty-graphics-protocol-image.data",
        "c:\\image.data",
        "C:image.data",
        "\\Windows\\Temp\\image.data",
        "/Windows/Temp/image.data",
        "image.data",
        ".zig-cache\\tmp\\image.data",
        "C:\\Device\\HarddiskVolume1\\image.data",
        "C:\\GLOBALROOT\\image.data",
        "C:\\dir\\console.png",
        "\\?\\image.data",
        "",
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i += 1) {
        ASSERT_TRUE(expectErrFor(Error::none, checkPath(accepted[i]), accepted[i]));
    }
}

TEST(kitty_windows, checkPath_rejects_reserved_device_names_in_any_component) {
    static const char *const reserved[] = {
        "CON",
        "con",
        "CON.png",
        "CON.tar.gz",
        "CON.",
        "CON  ",
        "CON .png",
        "NUL:stream",
        "PRN",
        "AUX",
        "nul",
        "COM1",
        "COM0",
        "COM9.rgba",
        "com5",
        "LPT1",
        "LPT9",
        "lpt0.png",
        /* Wisp: "COM\xC2\xB9" — the two WTF-8 bytes of U+00B9. C++ hex
         * escapes are greedy, so each is spelled in its own fragment. */
        "COM" "\xC2" "\xB9",
        "LPT" "\xC2" "\xB3" ".png",
        "C:\\Windows\\Temp\\NUL.rgba",
        "C:\\CON\\image.data",
        "dir/aux/image.data",
        "C:/Windows/Temp/com1",
        "..\\PRN",
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i += 1) {
        ASSERT_TRUE(
            expectErrFor(Error::ReservedDeviceName, checkPath(reserved[i]), reserved[i]));
    }

    static const char *const plain[] = {
        "CONSOLE",
        "CON1",
        "xCON",
        " CON",
        "COM",
        "COM10",
        "COMA",
        "COM" "\xC2" "\xB9" "\xC2" "\xB9",
        "COM" "\xC2" "\xB0",
        "LPT",
        "LPT.png",
        "conx.png",
        "C:",
        "C:\\",
        "image.data",
        "C:\\Windows\\Temp\\tty-graphics-protocol-image.data",
        "C:\\Windows\\Temp\\console.png",
        "",
    };
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i += 1) {
        ASSERT_TRUE(expectErrFor(Error::none, checkPath(plain[i]), plain[i]));
    }
}

TEST(kitty_windows, checkCanonicalPath_requires_a_drive_absolute_local_path) {
    ASSERT_TRUE(Error::none == checkCanonicalPath("C:\\Windows\\Temp\\image.data"));
    ASSERT_TRUE(Error::none == checkCanonicalPath("c:\\image.data"));

    /* Shapes GetFinalPathNameByHandle produces for files that are not
     * on a local drive-letter volume. */
    ASSERT_TRUE(Error::NotDriveAbsolute == checkCanonicalPath("\\\\server\\share\\image.data"));
    ASSERT_TRUE(Error::NotDriveAbsolute ==
                checkCanonicalPath("\\\\?\\UNC\\server\\share\\image.data"));
    ASSERT_TRUE(Error::NotDriveAbsolute ==
                checkCanonicalPath(
                    "\\\\?\\Volume{383da0b0-717f-41b6-8c36-00500992b58d}\\image.data"));

    /* The raw path checks still apply to the canonical form. */
    ASSERT_TRUE(Error::ReservedDeviceName == checkCanonicalPath("C:\\Windows\\Temp\\CON"));
}

TEST(kitty_windows, isDriveAbsolute) {
    ASSERT_TRUE(isDriveAbsolute("C:\\Windows\\Temp\\image.data"));
    ASSERT_TRUE(isDriveAbsolute("c:\\image.data"));
    ASSERT_TRUE(isDriveAbsolute("Z:\\"));

    ASSERT_TRUE(!isDriveAbsolute("\\\\server\\share\\image.data"));
    ASSERT_TRUE(!isDriveAbsolute("UNC\\server\\share\\image.data"));
    ASSERT_TRUE(!isDriveAbsolute("C:/Windows/image.data"));
    ASSERT_TRUE(!isDriveAbsolute("C:image.data"));
    ASSERT_TRUE(!isDriveAbsolute("\\Windows\\image.data"));
    ASSERT_TRUE(!isDriveAbsolute("1:\\image.data"));
    ASSERT_TRUE(!isDriveAbsolute("C:"));
    ASSERT_TRUE(!isDriveAbsolute(""));
}

/* Wisp: upstream's `defer anim.deinit(alloc)` — ASSERT_* returns from the
 * test on failure, so the free has to happen in a destructor or a failing
 * assertion would also leak and fail zz_Wisp_no_leaks. */
namespace {
struct AnimationGuard {
    Animation *anim;
    zigstd::Allocator alloc;
    AnimationGuard(Animation *a, zigstd::Allocator al) : anim(a), alloc(al) {}
    ~AnimationGuard() { anim->deinit(alloc); }
};
} /* namespace */

TEST(kitty_windows, animation_gap_helpers) {
    const zigstd::Allocator alloc = zigstd::testing_allocator();

    Animation anim;
    AnimationGuard guard(&anim, alloc);
    {
        Animation::Frame frame;
        frame.data = alloc.alloc(4);
        frame.data_len = 4;
        frame.gap_ms = 100;
        ASSERT_TRUE(anim.frames.append(alloc, frame));
    }

    ASSERT_TRUE(2 == anim.frameCount());
    ASSERT_TRUE(0 == anim.gapAt(0));
    ASSERT_TRUE(100 == anim.gapAt(1));
    ASSERT_TRUE(100 == anim.durationMs());
    ASSERT_TRUE(4 == anim.frameBytes());

    anim.setGapAt(0, 40);
    anim.setGapAt(1, 60);
    ASSERT_TRUE(100 == anim.durationMs());
    ASSERT_TRUE(40 == anim.root_gap_ms);
}

TEST(kitty_windows, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
