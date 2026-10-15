/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/graphics_exec.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * All 66 upstream tests, same names, same inputs, same assertions, in
 * upstream's order. Where an assertion differs it carries a `Wisp:` note
 * naming the upstream assertion it replaces.
 *
 * Wisp, the test-harness seams:
 *
 *   - `test_helpers.h`'s `TEST(suite, name)` takes a C identifier, not a
 *     string, so each upstream name is spelled with `: `, `(`, `)` and
 *     `=` dropped and spaces, `/` and `-` as underscores, as in the
 *     sibling port test files. `test "kittygfx more chunks with q=1"` is
 *     `TEST(kitty_graphics_exec, kittygfx_more_chunks_with_q1)`. No test
 *     is renamed beyond that mechanical transformation.
 *
 *   - `std.Io` is not carried over, so the `io` argument is dropped from
 *     every call (see graphics_exec.hpp).
 *
 *   - `execute` returns `?Response`, ported as a bool plus an out
 *     parameter, so `execute(...)` is the local `exec()` returning
 *     `{has, resp}`; `execute(...).?` is `exec(...)` plus
 *     `ASSERT_TRUE(e.has)`, and `execute(...) == null` is
 *     `ASSERT_TRUE(!exec(...).has)`.
 *
 *   - `command.Parser.parseString(alloc, str)` plus `defer
 *     cmd.deinit(alloc)` is the `CMD` macro below.
 *
 *   - `Response.message` is a pointer plus a length, and
 *     `ASSERT_STR_EQ` is a multi-statement macro that would dangle on a
 *     temporary `std::string`, so string comparisons go through
 *     `EXPECT_STR`, which binds a named local first.
 *
 *   - `testing.expectEqualSlices(u8, ...)` is `EXPECT_BYTES`, which
 *     checks the length and then `memcmp`s.
 *
 *   - `resp.encode(&writer)` writes into a `std::string` (see
 *     graphics_command.hpp); `writer.buffered()` is that string.
 *     Escapes are written `"\x1b" "_G"` because C++ hex escapes are
 *     greedy.
 *
 *   - `test_helpers.h`'s `ASSERT_EQ` casts both sides to `long`, which
 *     truncates 64-bit values and pointers, so generations, pointers and
 *     `maxInt(u32)`-scale values are compared with `ASSERT_TRUE(a == b)`.
 *
 *   - `ImageStorage.imageById` / `imageByNumber` return `Maybe<Image>`
 *     and `Image.data.bytes()` / `frameData()` are bool-plus-out-params;
 *     `img.animation` is the `animation_` pointer. `.?` becomes an
 *     explicit `ASSERT_TRUE(...)` on the presence flag.
 *
 *   - `Placement.Location.virtual` is `virtual_` and
 *     `Animation.frame_shown_at_ms: ?u64` is
 *     `has_frame_shown_at_ms` plus the value.
 *
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test.
 *
 * Wisp, assertions that differ (two tests, both noted at the site):
 *
 *   - `kittygfx test valid u32 (expect invalid image ID)` and
 *     `kittygfx test valid i32 (expect invalid image ID)` use
 *     `testing.expectEqual(resp.message, "ENOENT: image not found")`.
 *     Zig's `expectEqual` on two `[]const u8` compares pointer and
 *     length, which passes only because both operands are the same
 *     deduplicated string literal. The port compares the message
 *     content, which is the property the test is about.
 *
 * Wisp, nothing else is weakened: no test needed a PNG decoder (which is
 * a null hook in this configuration), so none was downgraded to
 * `UnsupportedFormat`.
 *
 * Wisp, coverage note: these tests reach six `graphics_storage.hpp`
 * functions that have no upstream tests of their own —
 * `convertImageToRgba`, `resolveParent`, `deleteAnimationFrame`,
 * `reserveAnimationBytes`, `releaseAnimationBytes` and
 * `markImageContentChanged`.
 */

#include <new>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_exec.hpp"
#include "../vt/screen_impl.hpp"
#include "../vt/terminal_impl.hpp"
#include "../zigstd/allocator.hpp"

using namespace wisp;
using namespace wisp::vt;

namespace ge = ::wisp::terminal::kitty::graphics_exec;
namespace gs = ::wisp::terminal::kitty::graphics_storage;
namespace gi = ::wisp::terminal::kitty::graphics_image;
namespace ganim = ::wisp::terminal::kitty::graphics_animation;
namespace gcmd = ::wisp::terminal::kitty::graphics;

typedef gs::ImageStorage ImageStorage;
typedef ImageStorage::Placement Placement;
typedef ImageStorage::PlacementKey PlacementKey;
typedef ImageStorage::PlacementId PlacementId;
typedef gi::Image Image;
typedef gcmd::Command Command;
typedef gcmd::Response Response;
typedef ganim::Animation Animation;
typedef PageList::Pin Pin;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: var t = try Terminal.init(io, alloc, .{ .rows = r, .cols = c });
 * defer t.deinit(alloc); */
struct TermHolder {
    Terminal t;
    bool ok;
    zigstd::Allocator alloc;
    explicit TermHolder(const Terminal::Options &o) : alloc(talloc()) {
        ok = Terminal::init(alloc, o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(alloc);
    }
};

#define TERM(v, c, r)                                                                              \
    Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                  \
    TermHolder v##_holder(v##_opts);                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    Terminal &v = v##_holder.t

/* Wisp: const cmd = try command.Parser.parseString(alloc, s);
 * defer cmd.deinit(alloc); */
struct CmdHolder {
    Command cmd;
    zigstd::Allocator alloc;
    gcmd::Error err;
    explicit CmdHolder(const char *s) : alloc(talloc()) {
        err = gcmd::Parser::parseString(alloc, (const uint8_t *)s, strlen(s), &cmd);
    }
    ~CmdHolder() {
        if (err == gcmd::Error::none) cmd.deinit(alloc);
    }
};

#define CMD(v, s)                                                                                  \
    CmdHolder v##_h(s);                                                                            \
    ASSERT_TRUE(v##_h.err == gcmd::Error::none);                                                    \
    Command &v = v##_h.cmd

/* Wisp: a directly constructed Command whose data we own. */
struct RawCmdHolder {
    Command cmd;
    zigstd::Allocator alloc;
    RawCmdHolder() : alloc(talloc()) {}
    ~RawCmdHolder() { cmd.deinit(alloc); }
};

/* Wisp: `?Response`. */
struct ExecResult {
    bool has;
    Response resp;
};

static ExecResult exec(Terminal *t, const Command *cmd) {
    ExecResult r;
    r.has = ge::execute(talloc(), t, cmd, &r.resp);
    return r;
}

/* Wisp: `ASSERT_STR_EQ` is multi-statement, so a temporary std::string
 * argument would be destroyed before the comparison. Bind a named local. */
#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

static std::string msg(const Response &r) { return std::string(r.message, r.message_len); }

/* Wisp: testing.expectEqualSlices(u8, expected, actual). */
#define EXPECT_BYTES(exp_ptr, exp_len, act_ptr, act_len)                                           \
    do {                                                                                           \
        ASSERT_EQ((long)(exp_len), (long)(act_len));                                               \
        ASSERT_EQ(0, memcmp((const void *)(exp_ptr), (const void *)(act_ptr),                      \
                            (size_t)(exp_len)));                                                   \
    } while (0)

/* ──────────────────────────────────────────────────────────────────────── */

TEST(kitty_graphics_exec, kittygfx_query_validates_image_data) {
    const zigstd::Allocator alloc = talloc();

    TERM(terminal, 5, 5);

    RawCmdHolder h;
    h.cmd.control.tag = Command::Action::query;
    h.cmd.control.query.format = gcmd::Transmission::Format::rgb;
    h.cmd.control.query.width = 1;
    h.cmd.control.query.height = 1;
    h.cmd.control.query.image_id = 31;
    /* A 1x1 RGB image requires three bytes. */
    {
        static const uint8_t data[] = {0, 0};
        h.cmd.data = alloc.dupe(data, sizeof data);
        ASSERT_NOT_NULL(h.cmd.data);
        h.cmd.data_len = sizeof data;
    }

    const ExecResult e = exec(&terminal, &h.cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    ASSERT_EQ(31, e.resp.id);
    EXPECT_STR("EINVAL: invalid data", msg(e.resp));
    ASSERT_EQ(0, terminal.screens.active->kitty_images.images.count());
}

TEST(kitty_graphics_exec, kittygfx_valid_query_does_not_replace_or_store_image) {
    const zigstd::Allocator alloc = talloc();

    TERM(terminal, 5, 5);
    ImageStorage *storage = &terminal.screens.active->kitty_images;

    /* Store a red pixel under the same ID used by the query. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=31;/wAA");
        const ExecResult e = exec(&terminal, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Successfully validate a black pixel without replacing the red one. */
    RawCmdHolder h;
    h.cmd.control.tag = Command::Action::query;
    h.cmd.control.query.format = gcmd::Transmission::Format::rgb;
    h.cmd.control.query.width = 1;
    h.cmd.control.query.height = 1;
    h.cmd.control.query.image_id = 31;
    {
        static const uint8_t data[] = {0, 0, 0};
        h.cmd.data = alloc.dupe(data, sizeof data);
        ASSERT_NOT_NULL(h.cmd.data);
        h.cmd.data_len = sizeof data;
    }

    const ExecResult e = exec(&terminal, &h.cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());
    ASSERT_EQ(1, storage->images.count());
    {
        const Maybe<Image> img = storage->imageById(31);
        ASSERT_TRUE(img.has);
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img.value.data.bytes(&got, &got_len));
        static const uint8_t expected[] = {255, 0, 0};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_image_id_and_number_are_mutually_exclusive_for_every_action) {
    TERM(t, 5, 5);

    static const char *const inputs[] = {
        "a=q,f=24,s=1,v=1,i=1,I=2,p=3;AAAA", "a=t,f=24,s=1,v=1,i=1,I=2,p=3;AAAA",
        "a=T,f=24,s=1,v=1,i=1,I=2,p=3;AAAA", "a=p,i=1,I=2,p=3",
        "a=d,d=a,i=1,I=2,p=3",               "a=f,i=1,I=2,p=3;AAAA",
        "a=a,i=1,I=2,p=3",                   "a=c,i=1,I=2,p=3",
    };

    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);

        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
        ASSERT_EQ(2, e.resp.image_number);
        ASSERT_EQ(3, e.resp.placement_id);
        EXPECT_STR("EINVAL: image ID and number are mutually exclusive", msg(e.resp));

        std::string writer;
        e.resp.encode(&writer);
        EXPECT_STR("\x1b"
                   "_Gi=1,I=2,p=3;EINVAL: image ID and number are mutually exclusive"
                   "\x1b"
                   "\\",
                   writer);
    }

    ASSERT_EQ(0, t.screens.active->kitty_images.images.count());
    ASSERT_EQ(0, t.screens.active->kitty_images.placements.count());
}

TEST(kitty_graphics_exec, kittygfx_conflicting_identifiers_are_rejected_before_mutation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Store an image so a buggy put would succeed and mutate placement state. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Directly constructed commands must receive the same validation as
     * commands produced by the parser. */
    {
        Command cmd;
        cmd.control.tag = Command::Action::display;
        cmd.control.display.image_id = 1;
        cmd.control.display.image_number = 2;
        cmd.control.display.placement_id = 7;
        cmd.control.display.cursor_movement = gcmd::Display::CursorMovement::none;
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
        ASSERT_EQ(2, e.resp.image_number);
        ASSERT_EQ(7, e.resp.placement_id);
        ASSERT_EQ(0, storage->placements.count());
    }

    /* Add a real placement so a buggy delete would remove it. */
    {
        CMD(cmd, "a=p,i=1,p=7,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, storage->placements.count());
    }

    {
        CMD(cmd, "a=d,d=i,i=1,I=2,p=7");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        ASSERT_EQ(1, storage->placements.count());
    }

    /* q=2 suppresses the required error response but not the validation. */
    {
        CMD(cmd, "a=d,d=i,i=1,I=2,p=7,q=2");
        ASSERT_TRUE(!exec(&t, &cmd).has);
        ASSERT_EQ(1, storage->placements.count());
    }
}

TEST(kitty_graphics_exec, kittygfx_chunked_success_response_uses_initial_identifiers) {
    TERM(t, 5, 5);

    {
        CMD(cmd, "a=t,f=24,s=1,v=2,I=93,p=7,m=1;AAAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    {
        CMD(cmd, "m=0;AAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);

        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
        ASSERT_EQ(93, e.resp.image_number);
        ASSERT_EQ(7, e.resp.placement_id);

        std::string writer;
        e.resp.encode(&writer);
        EXPECT_STR("\x1b"
                   "_Gi=1,I=93,p=7;OK"
                   "\x1b"
                   "\\",
                   writer);
    }
}

TEST(kitty_graphics_exec, kittygfx_chunked_error_response_uses_initial_identifiers) {
    TERM(t, 5, 5);

    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=41,p=7,m=1;AA==");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    {
        CMD(cmd, "m=0;AA==");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);

        ASSERT_TRUE(!e.resp.ok());
        ASSERT_EQ(41, e.resp.id);
        ASSERT_EQ(7, e.resp.placement_id);
        EXPECT_STR("EINVAL: invalid data", msg(e.resp));
    }
}

TEST(kitty_graphics_exec, kittygfx_more_chunks_with_q1) {
    TERM(t, 5, 5);

    /* Initial chunk has q=1 */
    {
        CMD(cmd, "a=T,f=24,t=d,i=1,s=1,v=2,c=10,r=1,m=1,q=1;////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    /* Subsequent chunk has no q but should respect initial */
    {
        CMD(cmd, "m=0;////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
}

TEST(kitty_graphics_exec, kittygfx_more_chunks_with_q0) {
    TERM(t, 5, 5);

    /* Initial chunk has q=0 */
    {
        CMD(cmd, "a=t,f=24,t=d,s=1,v=2,c=10,r=1,m=1,i=1,q=0;////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    /* Subsequent chunk has no q so should respond OK */
    {
        CMD(cmd, "m=0;////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
}

TEST(kitty_graphics_exec, kittygfx_more_chunks_with_chunk_increasing_q) {
    TERM(t, 5, 5);

    /* Initial chunk has q=0 */
    {
        CMD(cmd, "a=t,f=24,t=d,s=1,v=2,c=10,r=1,m=1,i=1,q=0;////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    /* Subsequent chunk sets q=1 so should not respond */
    {
        CMD(cmd, "m=0,q=1;////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
}

TEST(kitty_graphics_exec, kittygfx_delete_aborts_chunked_image_load) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Begin a two-chunk RGB image, then interrupt it with a delete. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=2,i=1,m=1;AAAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(storage->loading != nullptr);

    {
        CMD(cmd, "a=d");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(storage->loading == nullptr);

    /* A fresh chunked upload must start from empty state after the delete. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=2,i=1,m=1;AAAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    {
        CMD(cmd, "m=0;AAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        const Maybe<Image> img = storage->imageById(1);
        ASSERT_TRUE(img.has);
        ASSERT_EQ(6, img.value.data.len());
    }
}

TEST(kitty_graphics_exec, kittygfx_uppercase_id_delete_preserves_image_when_placement_does_not_match) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Store an unplaced 1x1 RGB image. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;AAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Uppercase deletion may free data only if the named placement matched. */
    {
        CMD(cmd, "a=d,d=I,i=1,p=7");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    ASSERT_TRUE(storage->imageById(1).has);
}

TEST(kitty_graphics_exec, kittygfx_default_format_is_rgba) {
    TERM(t, 5, 5);

    CMD(cmd, "a=t,t=d,i=1,s=1,v=2,c=10,r=1;///////////");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());

    ImageStorage *storage = &t.screens.active->kitty_images;
    const Maybe<Image> img = storage->imageById(1);
    ASSERT_TRUE(img.has);
    ASSERT_TRUE(gcmd::Transmission::Format::rgba == img.value.format);
}

TEST(kitty_graphics_exec, kittygfx_test_valid_u32_expect_invalid_image_ID) {
    TERM(t, 5, 5);

    CMD(cmd, "a=p,i=4294967295");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    /* Wisp: upstream's `expectEqual(resp.message, "ENOENT: image not
     * found")` compares a slice's pointer and length, which passes only
     * because both operands are the same deduplicated string literal.
     * The port compares the message content. */
    EXPECT_STR("ENOENT: image not found", msg(e.resp));
}

TEST(kitty_graphics_exec, kittygfx_test_valid_i32_expect_invalid_image_ID) {
    TERM(t, 5, 5);

    CMD(cmd, "a=p,i=1,z=-2147483648");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    /* Wisp: see the preceding test; upstream's `expectEqual` on two
     * `[]const u8` compares pointer and length. */
    EXPECT_STR("ENOENT: image not found", msg(e.resp));
}

TEST(kitty_graphics_exec, kittygfx_no_response_with_no_image_ID_or_number) {
    TERM(t, 5, 5);

    {
        CMD(cmd, "a=t,f=24,t=d,s=1,v=2,c=10,r=1,i=0,I=0;////////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
}

TEST(kitty_graphics_exec, kittygfx_no_response_with_no_image_ID_or_number_load_and_display) {
    TERM(t, 5, 5);

    {
        CMD(cmd, "a=T,f=24,t=d,s=1,v=2,c=10,r=1,i=0,I=0;////////");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
}

TEST(kitty_graphics_exec, kittygfx_retransmit_same_id_gets_fresh_image_generation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Transmit a 1x2 RGB image with id=1. */
    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=2;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    uint64_t gen1 = 0;
    {
        const Maybe<Image> img = storage->imageById(1);
        ASSERT_TRUE(img.has);
        gen1 = img.value.generation;
    }
    ASSERT_TRUE(gen1 > 0);
    ASSERT_TRUE(gen1 == storage->generation);

    /* Retransmit the same id with identical dimensions/length. The
     * (width, height, format, len) tuple is identical, so only the
     * generation can reveal that the contents were replaced. */
    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=2;AAAAAAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    uint64_t gen2 = 0;
    {
        const Maybe<Image> img = storage->imageById(1);
        ASSERT_TRUE(img.has);
        gen2 = img.value.generation;
    }
    ASSERT_TRUE(gen2 > gen1);
    ASSERT_TRUE(gen2 == storage->generation);
}

TEST(kitty_graphics_exec, kittygfx_retransmit_same_id_removes_existing_placements) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    /* Transmit and display an image, then add anonymous and named placements.
     * Multiple anonymous a=p placements for one image are explicitly valid. */
    {
        CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=2,C=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=1,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=1,p=7,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(3, storage->placements.count());
    ASSERT_EQ(tracked + 3, t.screens.active->pages.countTrackedPins());

    /* Retransmitting replaces the image and must delete every old placement.
     * Plain a=t creates no replacement placement of its own. */
    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=2;AAAAAAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());

    /* a=T creates one new placement after deleting the previous image and
     * placements, so repeated redraws remain bounded at one placement. */
    {
        CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=2,C=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=2,C=1;AAAAAAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(1, storage->placements.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_exec, kittygfx_retransmit_same_id_removes_image_on_first_chunk) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    /* Store and display the old image. */
    {
        CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=2,C=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_TRUE(storage->imageById(1).has);
    ASSERT_EQ(1, storage->placements.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    /* Starting a replacement for the same explicit ID removes the old image
     * and placements before the replacement's final chunk arrives. */
    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=2,m=1;AAAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(storage->loading != nullptr);
    ASSERT_TRUE(!storage->imageById(1).has);
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_exec, kittygfx_delete_then_retransmit_same_id_gets_fresh_generation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Transmit and display, then delete everything (including image
     * data), then retransmit the same ID. */
    {
        CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=2,c=1,r=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    uint64_t gen1 = 0;
    {
        const Maybe<Image> img = storage->imageById(1);
        ASSERT_TRUE(img.has);
        gen1 = img.value.generation;
    }

    {
        CMD(cmd, "a=d,d=A");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(!storage->imageById(1).has);
    const uint64_t gen_delete = storage->generation;
    ASSERT_TRUE(gen_delete > gen1);

    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=2;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    uint64_t gen2 = 0;
    {
        const Maybe<Image> img = storage->imageById(1);
        ASSERT_TRUE(img.has);
        gen2 = img.value.generation;
    }
    ASSERT_TRUE(gen2 > gen1);
    ASSERT_TRUE(gen2 > gen_delete);
}

TEST(kitty_graphics_exec, kittygfx_display_clamps_cell_offsets) {
    TERM(t, 5, 5);
    t.width_px = 50;   /* 10 px per col */
    t.height_px = 100; /* 20 px per row */

    CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=1,c=2,r=1,X=99,Y=99,C=1;AAAA");

    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());

    ImageStorage *storage = &t.screens.active->kitty_images;
    ImageStorage::PlacementMap::Iterator it = storage->placements.iterator();
    ImageStorage::PlacementMap::Entry entry;
    ASSERT_TRUE(it.next(&entry));
    const Placement *placement = entry.value_ptr;
    ASSERT_EQ(9, placement->x_offset);
    ASSERT_EQ(19, placement->y_offset);

    const Maybe<Image> img = storage->imageById(1);
    ASSERT_TRUE(img.has);
    const Placement::PixelSize actual = placement->pixelSize(img.value, &t);
    ASSERT_EQ(11, actual.width);
    ASSERT_EQ(1, actual.height);
}

TEST(kitty_graphics_exec, kittygfx_placement_bounds_cursor_movement_for_untrusted_dimensions) {
    TERM(t, 5, 5);

    CMD(cmd, "a=T,t=d,f=24,i=1,s=1,v=1,c=4294967295,r=4294967295;////");

    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());
    ASSERT_EQ(1, t.screens.active->kitty_images.placements.count());
    /* Reaching the bottom takes four rows, then scrolling is capped to one
     * screen (five rows), matching Kitty's bounded screen-scroll behavior. */
    ASSERT_EQ(10, t.screens.active->pages.scrollbar().total);
}

TEST(kitty_graphics_exec, kittygfx_placement_moves_cursor_past_a_tall_image) {
    TERM(t, 5, 5);

    /* Load a one-pixel RGB image, then place it over the full width and eight
     * rows. Eight rows are taller than the screen but still within Kitty's
     * scroll budget. */
    {
        CMD(cmd, "a=t,t=d,f=24,i=1,s=1,v=1;////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=1,p=1,c=5,r=8");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* A subsequent placement begins immediately after the first one instead
     * of overlapping it at the old one-screen movement cap. */
    {
        CMD(cmd, "a=p,i=1,p=2,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    ImageStorage *storage = &t.screens.active->kitty_images;
    Placement first;
    ASSERT_TRUE(storage->placements.get(PlacementKey(1, PlacementId::external(1)), &first));
    Placement second;
    ASSERT_TRUE(storage->placements.get(PlacementKey(1, PlacementId::external(2)), &second));
    /* Wisp: upstream's `switch (location) { .pin => |pin| pin, .virtual,
     * .relative => unreachable }`. */
    ASSERT_TRUE(first.location.tag == Placement::Location::Tag::pin);
    ASSERT_TRUE(second.location.tag == Placement::Location::Tag::pin);
    Pin *first_pin = first.location.pin;
    Pin *second_pin = second.location.pin;
    const Maybe<point::Point> first_pt =
        t.screens.active->pages.pointFromPin(point::Tag::screen, *first_pin);
    ASSERT_TRUE(first_pt.has);
    const Maybe<point::Point> second_pt =
        t.screens.active->pages.pointFromPin(point::Tag::screen, *second_pin);
    ASSERT_TRUE(second_pt.has);
    ASSERT_EQ(first_pt.value.c.y + 8, second_pt.value.c.y);
}

TEST(kitty_graphics_exec, kittygfx_unknown_format_responds_with_EINVAL) {
    TERM(t, 5, 5);

    CMD(cmd, "a=t,f=42,t=d,i=31,s=1,v=1;AAAA");

    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    ASSERT_EQ(31, e.resp.id);
    EXPECT_STR("EINVAL: unsupported format", msg(e.resp));
    ASSERT_EQ(0, t.screens.active->kitty_images.images.count());
}

TEST(kitty_graphics_exec, kittygfx_unknown_format_on_query_responds_with_EINVAL) {
    TERM(t, 5, 5);

    CMD(cmd, "a=q,f=42,t=d,i=31,s=1,v=1;AAAA");

    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    ASSERT_EQ(31, e.resp.id);
    EXPECT_STR("EINVAL: unsupported format", msg(e.resp));
}

TEST(kitty_graphics_exec, kittygfx_zero_format_is_rgba) {
    TERM(t, 5, 5);

    /* Kitty treats f=0 as an absent f, i.e. RGBA. */
    CMD(cmd, "a=t,f=0,t=d,i=1,s=1,v=2,c=10,r=1;///////////");

    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());

    const Maybe<Image> img = t.screens.active->kitty_images.imageById(1);
    ASSERT_TRUE(img.has);
    ASSERT_TRUE(gcmd::Transmission::Format::rgba == img.value.format);
}

TEST(kitty_graphics_exec, kittygfx_unknown_format_with_q3_has_no_response) {
    TERM(t, 5, 5);

    /* q above 2 is out of range in the spec but Kitty suppresses
     * everything for anything above 1, so we must still parse it. */
    CMD(cmd, "a=t,f=42,t=d,i=31,q=3,s=1,v=1;AAAA");

    ASSERT_TRUE(!exec(&t, &cmd).has);
}

TEST(kitty_graphics_exec, kittygfx_out_of_range_display_keys_are_tolerated) {
    TERM(t, 5, 5);

    {
        CMD(cmd, "a=T,f=24,t=d,i=31,s=1,v=1,C=2,U=2;AAAA");

        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(31, e.resp.id);
    }

    /* U=2 must behave like U=1, so the placement is virtual. */
    ImageStorage *storage = &t.screens.active->kitty_images;
    ImageStorage::PlacementMap::Iterator it = storage->placements.iterator();
    ImageStorage::PlacementMap::Entry entry;
    ASSERT_TRUE(it.next(&entry));
    ASSERT_TRUE(entry.value_ptr->location.tag == Placement::Location::Tag::virtual_);
}

TEST(kitty_graphics_exec, kittygfx_number_based_transmission_assigns_smallest_free_id) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Empty storage: the first free ID is 1, as in Kitty. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,I=42;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
        ASSERT_EQ(42, e.resp.image_number);
    }

    /* Occupy ID 2 explicitly; the next number gets 3. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=2;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,I=43;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(3, e.resp.id);
    }

    /* Deleting ID 1 opens a gap that the next number fills. */
    {
        CMD(cmd, "a=d,d=I,i=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,I=44;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
    }

    ASSERT_EQ(3, storage->images.count());
    {
        const Maybe<Image> i1 = storage->imageById(1);
        ASSERT_TRUE(i1.has);
        ASSERT_EQ(44, i1.value.number);
        const Maybe<Image> i2 = storage->imageById(2);
        ASSERT_TRUE(i2.has);
        ASSERT_EQ(0, i2.value.number);
        const Maybe<Image> i3 = storage->imageById(3);
        ASSERT_TRUE(i3.has);
        ASSERT_EQ(43, i3.value.number);
    }
}

TEST(kitty_graphics_exec, kittygfx_number_based_id_assignment_does_not_replace_client_image) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* A client stores and places a red pixel on the ID the old wrapping
     * counter would assign first. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=2147483647;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=2147483647,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* A number-based transmission must not collide with it. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,I=42;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
    }

    /* The client's image and placement are untouched. */
    ASSERT_EQ(2, storage->images.count());
    ASSERT_EQ(1, storage->placements.count());
    {
        const Maybe<Image> img = storage->imageById(2147483647u);
        ASSERT_TRUE(img.has);
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img.value.data.bytes(&got, &got_len));
        static const uint8_t expected[] = {255, 0, 0};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_implicit_id_assignment_does_not_replace_client_image) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* A client stores and places a red pixel on the ID the implicit
     * counter assigns first. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=2147483647;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=2147483647,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Transmit without an ID or number: no response, and the counter
     * skips over the in-use ID instead of replacing that image. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1;AAD/");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }

    ASSERT_EQ(2, storage->images.count());
    ASSERT_EQ(1, storage->placements.count());
    {
        const Maybe<Image> img = storage->imageById(2147483647u);
        ASSERT_TRUE(img.has);
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img.value.data.bytes(&got, &got_len));
        static const uint8_t expected[] = {255, 0, 0};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
    {
        const Maybe<Image> implicit = storage->imageById(2147483648u);
        ASSERT_TRUE(implicit.has);
        ASSERT_TRUE(implicit.value.metadata.implicit_id);
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(implicit.value.data.bytes(&got, &got_len));
        static const uint8_t expected[] = {0, 0, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_with_missing_parent_image) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;AAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* The parent image does not exist: the placement must be rejected
     * with ENOPARENT, nothing may be created, and the cursor must not
     * move (a relative placement never moves the cursor, and a failed
     * one certainly must not). */
    {
        CMD(cmd, "a=p,i=1,p=1,P=42,Q=1,H=2,V=2");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ENOPARENT: parent image not found", msg(e.resp));
    }

    ASSERT_EQ(0, storage->placements.count());
    ASSERT_EQ(0, t.screens.active->cursor.x);
    ASSERT_EQ(0, t.screens.active->cursor.y);
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_with_missing_parent_placement) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=t,f=24,s=1,v=1,i=2;AAAA",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* The parent image exists but has no placements at all. */
    {
        CMD(cmd, "a=p,i=1,P=2");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ENOPARENT: parent placement not found", msg(e.resp));
    }

    /* The parent image has a placement, but not the requested one. */
    {
        CMD(cmd, "a=p,i=2,p=1,C=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=p,i=1,P=2,Q=9");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ENOPARENT: parent placement not found", msg(e.resp));
    }

    ASSERT_EQ(1, storage->placements.count());
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_cannot_parent_itself) {
    TERM(t, 5, 5);

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Explicitly via Q, and implicitly when the Q=0 fallback selects
     * the placement being replaced. */
    static const char *const inputs[] = {
        "a=p,i=1,p=1,P=1,Q=1",
        "a=p,i=1,p=1,P=1",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("EINVAL: placement cannot be its own parent", msg(e.resp));
    }

    /* The original placement must be untouched. */
    ImageStorage *storage = &t.screens.active->kitty_images;
    Placement p;
    ASSERT_TRUE(storage->placements.get(PlacementKey(1, PlacementId::external(1)), &p));
    ASSERT_TRUE(p.location.tag == Placement::Location::Tag::pin);
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_cycle) {
    TERM(t, 5, 5);

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=1,p=2,P=1,Q=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Replacing placement 1 with a parent of placement 2 would create
     * the cycle 1 -> 2 -> 1. */
    {
        CMD(cmd, "a=p,i=1,p=1,P=1,Q=2");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ECYCLE: parent chain creates a cycle", msg(e.resp));
    }

    /* The original placement must be untouched, keeping the stored
     * chains acyclic. */
    ImageStorage *storage = &t.screens.active->kitty_images;
    Placement p;
    ASSERT_TRUE(storage->placements.get(PlacementKey(1, PlacementId::external(1)), &p));
    ASSERT_TRUE(p.location.tag == Placement::Location::Tag::pin);
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_chain_depth_limit) {
    TERM(t, 5, 5);

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Chains of up to parent_chain_limit (8) links must work: these
     * placements form the chain 9 -> 8 -> ... -> 2 -> 1. */
    for (unsigned id = 2; id < 10; id += 1) {
        char buf[64];
        snprintf(buf, sizeof buf, "a=p,i=1,p=%u,P=1,Q=%u", id, id - 1);
        CmdHolder h(buf);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* One more link exceeds the limit. */
    {
        CMD(cmd, "a=p,i=1,p=10,P=1,Q=9");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ETOODEEP: parent chain too deep", msg(e.resp));
    }

    ImageStorage *storage = &t.screens.active->kitty_images;
    ASSERT_EQ(9, storage->placements.count());
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_does_not_move_cursor) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* C is left at its default (move after) on the relative placement
     * but it must never move the cursor. */
    static const char *const inputs[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=1,p=2,P=1,Q=1,H=3,V=2,c=2,r=2",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(0, t.screens.active->cursor.x);
    ASSERT_EQ(0, t.screens.active->cursor.y);

    /* The stored placement carries the parent link and offsets. */
    Placement p;
    ASSERT_TRUE(storage->placements.get(PlacementKey(1, PlacementId::external(2)), &p));
    ASSERT_TRUE(p.location.tag == Placement::Location::Tag::relative);
    const Placement::Relative rel = p.location.relative;
    ASSERT_TRUE(rel.parent.eql(PlacementKey(1, PlacementId::external(1))));
    ASSERT_EQ(3, rel.horizontal_offset);
    ASSERT_EQ(2, rel.vertical_offset);
}

TEST(kitty_graphics_exec, kittygfx_virtual_placement_with_parent_rejected_before_image_lookup) {
    TERM(t, 5, 5);

    /* Kitty rejects U=1 + P= before checking that the image exists,
     * so this must not be ENOENT. */
    CMD(cmd, "a=p,i=42,U=1,P=1");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    EXPECT_STR("EINVAL: virtual placement cannot refer to a parent", msg(e.resp));
}

TEST(kitty_graphics_exec, kittygfx_deleting_a_parent_deletes_relative_placements_transitively) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Root (pin) <- child <- grandchild */
    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=1,p=2,P=1,Q=1",
        "a=p,i=1,p=3,P=1,Q=2",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(3, storage->placements.count());

    /* Deleting the middle placement takes the grandchild with it but
     * leaves the root. */
    {
        CMD(cmd, "a=d,d=i,i=1,p=2");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_EQ(1, storage->placements.count());
    ASSERT_TRUE(storage->placements.contains(PlacementKey(1, PlacementId::external(1))));

    /* Deleting the root removes the remaining placement, and the
     * image itself is retained (lowercase delete). */
    {
        CMD(cmd, "a=d,d=i,i=1,p=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_TRUE(storage->imageById(1).has);
}

TEST(kitty_graphics_exec, kittygfx_retransmitting_parent_image_deletes_relative_placements) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Image 2's placement is relative to image 1's placement. */
    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=t,f=24,s=1,v=1,i=2;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=2,p=1,P=1,Q=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(2, storage->placements.count());

    /* Retransmitting image 1 removes its placements, which orphans
     * and removes image 2's placement too. Image 2 is left without
     * any placement so it is freed as well: retransmission deletes
     * with uppercase semantics, and kitty also removes an image whose
     * last placement died from a broken parent chain. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;AAAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_TRUE(!storage->imageById(2).has);
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_parent_fallback_picks_lowest_external_id) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA", "a=t,f=24,s=1,v=1,i=2;AAAA", "a=p,i=1,p=7,C=1",
        "a=p,i=1,p=3,C=1",           "a=p,i=2,P=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Wisp: upstream's `while (it.next()) |entry| { ... } else return
     * error.PlacementNotFound;` — a missing placement fails the test. */
    bool found = false;
    Placement::Relative rel;
    {
        ImageStorage::PlacementMap::Iterator it = storage->placements.iterator();
        ImageStorage::PlacementMap::Entry entry;
        while (it.next(&entry)) {
            if (entry.key_ptr->image_id == 2) {
                ASSERT_TRUE(entry.value_ptr->location.tag == Placement::Location::Tag::relative);
                rel = entry.value_ptr->location.relative;
                found = true;
                break;
            }
        }
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(rel.parent.eql(PlacementKey(1, PlacementId::external(3))));
}

TEST(kitty_graphics_exec, kittygfx_placements_created_after_a_full_reset_are_retained) {
    /* Regression test: Screen.reset reuses the cursor's tracked pin
     * but PageList.reset marked it garbage. Placements copy the cursor
     * pin, so every placement created after a reset was born garbage
     * and silently swept by the next placement command. */
    TERM(t, 80, 24);
    ImageStorage *storage = &t.screens.active->kitty_images;

    t.fullReset();

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=1,p=2,C=1",
        "a=p,i=1,p=3,P=1,Q=2",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(3, storage->placements.count());
}

TEST(kitty_graphics_exec, kittygfx_relative_placement_with_pruned_parent_is_ENOPARENT) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* The parent's tracked content is pruned from history but the
     * placement hasn't been swept yet. The put must reap it and answer
     * ENOPARENT rather than storing an orphan against it. */
    Placement *parent = storage->placements.getPtr(PlacementKey(1, PlacementId::external(1)));
    ASSERT_NOT_NULL(parent);
    ASSERT_TRUE(parent->location.tag == Placement::Location::Tag::pin);
    parent->location.pin->garbage = true;

    {
        CMD(cmd, "a=p,i=1,p=2,P=1,Q=1");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR("ENOPARENT: parent placement not found", msg(e.resp));
    }
    ASSERT_EQ(0, storage->placements.count());
}

TEST(kitty_graphics_exec, kittygfx_uppercase_delete_frees_image_of_cascaded_placements) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    static const char *const setup[] = {
        "a=t,f=24,s=1,v=1,i=1;AAAA",
        "a=p,i=1,p=1,C=1",
        "a=p,i=1,p=2,P=1,Q=1",
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i += 1) {
        CmdHolder h(setup[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* The uppercase delete only matches placement 1. The cascade
     * removes placement 2, leaving the image without placements, so
     * the image must be freed too. */
    {
        CMD(cmd, "a=d,d=I,i=1,p=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_TRUE(!storage->imageById(1).has);
}

/* Animation tests frequently start from a 1x1 RGB red image with
 * id=1 ("/wAA" is FF0000). Composition converts images to RGBA, so
 * after the first frame command the base data is FF0000FF. */

TEST(kitty_graphics_exec, kittygfx_animation_new_frame_with_default_gap_responds_with_frame_number) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Transmit a 1x1 RGB blue frame ("AAD/" is 0000FF). */
    CMD(cmd, "a=f,i=1,f=24,s=1,v=1;AAD/");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());
    ASSERT_EQ(1, e.resp.id);
    ASSERT_EQ(2, e.resp.frame);

    {
        std::string writer;
        e.resp.encode(&writer);
        EXPECT_STR("\x1b"
                   "_Gi=1,r=2;OK"
                   "\x1b"
                   "\\",
                   writer);
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    ASSERT_TRUE(gcmd::Transmission::Format::rgba == img->format);
    {
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img->data.bytes(&got, &got_len));
        static const uint8_t expected[] = {255, 0, 0, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }

    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(2, anim->frameCount());
    ASSERT_EQ(0, anim->root_gap_ms);
    ASSERT_EQ(40, anim->frames.items[0].gap_ms);
    {
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                     anim->frames.items[0].data_len);
    }

    /* The displayed frame is still the root: renderData is the base. */
    {
        const Image::Data rd = img->renderData();
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(rd.bytes(&got, &got_len));
        static const uint8_t expected[] = {255, 0, 0, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }

    /* Frame bytes count against the storage total. */
    ASSERT_EQ(8, storage->total_bytes);
}

TEST(kitty_graphics_exec, kittygfx_animation_frame_gap_normalization_on_create) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    static const char *const inputs[] = {
        "a=f,i=1,f=24,s=1,v=1,z=100;AAD/",
        "a=f,i=1,f=24,s=1,v=1,z=-5;AAD/",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(100, anim->frames.items[0].gap_ms);
    ASSERT_EQ(0, anim->frames.items[1].gap_ms);
}

TEST(kitty_graphics_exec, kittygfx_animation_background_fill_and_offset_composition) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* 2x1 RGB white image ("////////" is FFFFFF FFFFFF). */
    {
        CMD(cmd, "a=t,f=24,s=2,v=1,i=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* New frame: 1x1 RGB white transmitted at x=1 over an opaque red
     * background (Y=4278190335 is 0xff0000ff, R in the MSB). */
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1,x=1,Y=4278190335;////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    static const uint8_t expected[] = {255, 0, 0, 255, 255, 255, 255, 255};
    EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                 anim->frames.items[0].data_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_create_from_base_frame_with_overwrite) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Frame 2 based on the root (c=1) with a semi-transparent blue
     * pixel ("AAD/gA==" is 0000FF80) in overwrite mode: the source
     * replaces the canvas including alpha. */
    {
        CMD(cmd, "a=f,i=1,f=32,s=1,v=1,c=1,X=1;AAD/gA==");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    static const uint8_t expected[] = {0, 0, 255, 128};
    EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                 anim->frames.items[0].data_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_alpha_blend_composes_over_base_frame) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Blend a fully opaque blue pixel over the root: alpha blending
     * an opaque source is equivalent to a copy. */
    {
        CMD(cmd, "a=f,i=1,f=32,s=1,v=1,c=1;AAD//w==");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Blend a fully transparent pixel over the root: the canvas is
     * unchanged (still the red base). */
    {
        CMD(cmd, "a=f,i=1,f=32,s=1,v=1,c=1;AAD/AA==");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    {
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                     anim->frames.items[0].data_len);
    }
    {
        static const uint8_t expected[] = {255, 0, 0, 255};
        EXPECT_BYTES(expected, sizeof expected, anim->frames.items[1].data,
                     anim->frames.items[1].data_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_animation_edit_root_frame_bumps_generation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    uint64_t gen1 = 0;
    {
        Image *img = storage->imagePtrByIdOrNumber(1, 0);
        ASSERT_NOT_NULL(img);
        gen1 = img->generation;
    }

    /* r=1 edits the root frame in place. The root is the displayed
     * frame so the image generation must change. */
    CMD(cmd, "a=f,i=1,f=24,s=1,v=1,r=1;AAD/");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(e.resp.ok());
    ASSERT_EQ(1, e.resp.frame);

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    {
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img->data.bytes(&got, &got_len));
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
    ASSERT_TRUE(img->generation > gen1);
}

TEST(kitty_graphics_exec, kittygfx_animation_edit_frame_gap_without_data_change) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Create frame 2, then edit it with z=-1: gap becomes gapless.
     * Editing with z=0 leaves the gap unchanged (no 40ms default). */
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1,z=77;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1,r=2;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(77, anim->frames.items[0].gap_ms);

    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1,r=2,z=-1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(0, anim->frames.items[0].gap_ms);
}

TEST(kitty_graphics_exec, kittygfx_animation_frame_errors) {
    TERM(t, 5, 5);
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    struct Case {
        const char *input;
        const char *message;
    };
    static const Case cases[] = {
        /* Unknown image. */
        {"a=f,i=9,f=24,s=1,v=1;AAD/", "ENOENT: image not found"},
        /* Frame bigger than the image. */
        {"a=f,i=1,f=24,s=2,v=1;////////", "EINVAL: frame dimensions exceed image"},
        /* Nonexistent base frame. */
        {"a=f,i=1,f=24,s=1,v=1,c=5;AAD/", "EINVAL: base frame not found"},
        /* Insufficient data for the declared rectangle. */
        {"a=f,i=1,f=24,s=1,v=1;AA==", "ENODATA: insufficient data"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        CmdHolder h(cases[i].input);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR(cases[i].message, msg(e.resp));
    }

    /* None of the failures may have created a frame. */
    ImageStorage *storage = &t.screens.active->kitty_images;
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    if (img->animation_ != nullptr) {
        ASSERT_EQ(0, img->animation_->frames.items_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_animation_excess_frame_data_is_truncated) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Six bytes for a 1x1 RGB frame: Kitty tolerates the excess. */
    CMD(cmd, "a=f,i=1,f=24,s=1,v=1;AAD/////");
    {
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    static const uint8_t expected[] = {0, 0, 255, 255};
    EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                 anim->frames.items[0].data_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_chunked_frame_transmission) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* 2x1 RGB white image. */
    {
        CMD(cmd, "a=t,f=24,s=2,v=1,i=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Transmit a 2x1 frame in two chunks of one pixel each. The
     * chunks repeat a=f per the protocol. No response until the
     * final chunk. */
    {
        CMD(cmd, "a=f,i=1,f=24,s=2,v=1,z=60,m=1;/wAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(storage->loading != nullptr);
    {
        CMD(cmd, "a=f,m=0;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(1, e.resp.id);
        ASSERT_EQ(2, e.resp.frame);
    }
    ASSERT_TRUE(storage->loading == nullptr);

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(60, anim->frames.items[0].gap_ms);
    static const uint8_t expected[] = {255, 0, 0, 255, 0, 0, 255, 255};
    EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                 anim->frames.items[0].data_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_chunked_frame_continuation_without_af) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    {
        CMD(cmd, "a=t,f=24,s=2,v=1,i=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* The spec requires chunks to repeat a=f, but a bare continuation
     * (which parses as a transmit action) must still continue the
     * frame load rather than turn into an image transmission. */
    {
        CMD(cmd, "a=f,i=1,f=24,s=2,v=1,m=1;/wAA");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    {
        CMD(cmd, "m=0;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(2, e.resp.frame);
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(1, anim->frames.items_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_control_command_sets_gap_current_state_and_loops) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    const uint64_t gen1 = img->generation;

    /* One command can set several things: the root frame's gap
     * (r=1,z=50), the current frame (c=2), the state (s=3, running),
     * and the loop count (v=3, meaning two loops). Successful a=a
     * commands never respond. */
    CMD(cmd, "a=a,i=1,r=1,z=50,c=2,s=3,v=3");
    ASSERT_TRUE(!exec(&t, &cmd).has);

    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(50, anim->root_gap_ms);
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(Animation::State::running == anim->state);
    ASSERT_EQ(2, anim->max_loops);

    /* Changing the current frame changes the displayed content. */
    ASSERT_TRUE(img->generation > gen1);
    {
        const Image::Data rd = img->renderData();
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(rd.bytes(&got, &got_len));
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_animation_control_command_ignores_invalid_values) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Out-of-range frame numbers and s values are silently ignored. */
    CMD(cmd, "a=a,i=1,r=9,z=50,c=9,s=7");
    ASSERT_TRUE(!exec(&t, &cmd).has);

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(0, anim->root_gap_ms);
    ASSERT_EQ(0, anim->current_index);
    ASSERT_TRUE(Animation::State::stopped == anim->state);
}

TEST(kitty_graphics_exec, kittygfx_animation_control_command_missing_image_responds_ENOENT) {
    TERM(t, 5, 5);

    CMD(cmd, "a=a,i=9,s=3");
    const ExecResult e = exec(&t, &cmd);
    ASSERT_TRUE(e.has);
    ASSERT_TRUE(!e.resp.ok());
    EXPECT_STR("ENOENT: image not found", msg(e.resp));
}

TEST(kitty_graphics_exec, kittygfx_animation_compose_frames) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* 2x1 RGB image: red then blue ("/wAAAAD/" is FF0000 0000FF). */
    {
        CMD(cmd, "a=t,f=24,s=2,v=1,i=1;/wAAAAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    /* Frame 2 starts as a transparent canvas with a white pixel at
     * x=0 ("////" is FFFFFF). */
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1;////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Compose the root's pixel at (1,0) onto frame 2 at (0,0):
     * r=1 is the source frame, c=2 the destination, X/Y the source
     * offsets, x/y the destination offsets. */
    {
        CMD(cmd, "a=c,i=1,r=1,c=2,w=1,h=1,X=1,x=0");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
        ASSERT_EQ(0, e.resp.frame);
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    static const uint8_t expected[] = {0, 0, 255, 255, 0, 0, 0, 0};
    EXPECT_BYTES(expected, sizeof expected, anim->frames.items[0].data,
                 anim->frames.items[0].data_len);
}

TEST(kitty_graphics_exec, kittygfx_animation_compose_current_frame_bumps_generation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* 2x1 RGB white image; the root frame is the displayed frame. */
    {
        CMD(cmd, "a=t,f=24,s=2,v=1,i=1;////////");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    const uint64_t gen1 = img->generation;

    /* a=c works on images without animation state: both frames are
     * the root. Copy pixel 0 onto pixel 1. */
    CMD(cmd, "a=c,i=1,r=1,c=1,w=1,h=1,x=1");
    {
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_TRUE(img->generation > gen1);
}

TEST(kitty_graphics_exec, kittygfx_animation_compose_errors) {
    TERM(t, 5, 5);
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    struct Case {
        const char *input;
        const char *message;
    };
    static const Case cases[] = {
        {"a=c,i=9,r=1,c=1", "ENOENT: image not found"},
        {"a=c,i=1,r=2,c=1", "ENOENT: source frame not found"},
        {"a=c,i=1,r=1,c=2", "ENOENT: destination frame not found"},
        /* 1x1 image: any offset pushes the rect out of bounds. */
        {"a=c,i=1,r=1,c=1,x=1", "EINVAL: destination rectangle out of bounds"},
        {"a=c,i=1,r=1,c=1,X=1", "EINVAL: source rectangle out of bounds"},
        /* Same frame, same (whole-image) rect: overlap. */
        {"a=c,i=1,r=1,c=1", "EINVAL: source and destination rectangles overlap"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        CmdHolder h(cases[i].input);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(!e.resp.ok());
        EXPECT_STR(cases[i].message, msg(e.resp));
    }
}

TEST(kitty_graphics_exec, kittygfx_animation_delete_frame_promotes_root_and_fixes_current) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    /* Frames 2 and 3: blue and white. */
    static const char *const inputs[] = {
        "a=f,i=1,f=24,s=1,v=1,z=10;AAD/",
        "a=f,i=1,f=24,s=1,v=1,z=20;////",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        const ExecResult e = exec(&t, &h.cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    const size_t total_before = storage->total_bytes;

    /* Delete the root frame (r omitted selects it): frame 2 becomes
     * the new root, including its gap. Deletes never respond. */
    {
        CMD(cmd, "a=d,d=f,i=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(2, anim->frameCount());
    ASSERT_EQ(10, anim->root_gap_ms);
    {
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(img->data.bytes(&got, &got_len));
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
    ASSERT_EQ(total_before - 4, storage->total_bytes);

    /* Delete a frame number past the end: clamps to the last frame. */
    {
        CMD(cmd, "a=d,d=f,i=1,r=9");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_EQ(1, anim->frameCount());
    ASSERT_EQ(total_before - 8, storage->total_bytes);

    /* Down to one frame: d=f is now a no-op and the image survives. */
    {
        CMD(cmd, "a=d,d=f,i=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(storage->imageById(1).has);
}

TEST(kitty_graphics_exec, kittygfx_animation_uppercase_frame_delete_removes_non_animated_image) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;

    /* Transmit and place: unlike d=I, an uppercase frame delete on a
     * non-animated image removes it even though it is placed. */
    {
        CMD(cmd, "a=T,f=24,s=1,v=1,i=1,C=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(1, storage->placements.count());

    {
        CMD(cmd, "a=d,d=F,i=1");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_TRUE(!storage->imageById(1).has);
    ASSERT_EQ(0, storage->placements.count());
    ASSERT_EQ(0, storage->total_bytes);
}

TEST(kitty_graphics_exec, kittygfx_animation_deleting_current_frame_refreshes_display) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    static const char *const inputs[] = {
        "a=f,i=1,f=24,s=1,v=1;AAD/",
        "a=f,i=1,f=24,s=1,v=1;////",
        "a=a,i=1,c=3",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i += 1) {
        CmdHolder h(inputs[i]);
        ASSERT_TRUE(h.err == gcmd::Error::none);
        (void)exec(&t, &h.cmd); /* Wisp: upstream's `_ = execute(...)` */
    }

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(2, anim->current_index);
    const uint64_t gen1 = img->generation;

    /* Deleting the last frame (the current one) clamps the index and
     * refreshes the display. */
    {
        CMD(cmd, "a=d,d=f,i=1,r=3");
        ASSERT_TRUE(!exec(&t, &cmd).has);
    }
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(img->generation > gen1);
    {
        const Image::Data rd = img->renderData();
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(rd.bytes(&got, &got_len));
        static const uint8_t expected[] = {0, 0, 255, 255};
        EXPECT_BYTES(expected, sizeof expected, got, got_len);
    }
}

TEST(kitty_graphics_exec, kittygfx_animation_retransmitting_base_image_resets_animation) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    ASSERT_EQ(8, storage->total_bytes);

    /* Retransmit the base image: the animation is dropped and the
     * frame bytes are credited back. */
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    ASSERT_TRUE(img->animation_ == nullptr);
    ASSERT_EQ(3, storage->total_bytes);
}

TEST(kitty_graphics_exec, kittygfx_animation_control_negative_gap_makes_frame_gapless) {
    TERM(t, 5, 5);
    ImageStorage *storage = &t.screens.active->kitty_images;
    {
        CMD(cmd, "a=t,f=24,s=1,v=1,i=1;/wAA");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }
    {
        CMD(cmd, "a=f,i=1,f=24,s=1,v=1;AAD/");
        const ExecResult e = exec(&t, &cmd);
        ASSERT_TRUE(e.has);
        ASSERT_TRUE(e.resp.ok());
    }

    CMD(cmd, "a=a,i=1,r=2,z=-1");
    ASSERT_TRUE(!exec(&t, &cmd).has);

    Image *img = storage->imagePtrByIdOrNumber(1, 0);
    ASSERT_NOT_NULL(img);
    Animation *anim = img->animation_;
    ASSERT_NOT_NULL(anim);
    ASSERT_EQ(0, anim->frames.items[0].gap_ms);
}

TEST(kitty_graphics_exec, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
