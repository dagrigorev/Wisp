/* Tests that escape sequences arriving from the pty actually reach the
 * ported graphics code -- the wiring, not the parsers or the decoders,
 * which have their own suites (test_kitty_graphics_command_port.cpp,
 * test_kitty_graphics_exec_port.cpp, test_sixel.cpp).
 *
 * Everything here drives `terminal::stream::Stream<stream_terminal::Handler>`
 * with raw bytes and then asserts on terminal state and on what was written
 * back to the pty. Nothing calls `execute` or `sixel::decode` through the
 * front door except the one sixel test that needs a reference image to
 * compare against.
 *
 * Two paths are covered:
 *
 *   - The APC Kitty path, `ESC _ G <command> ESC \`. This is a
 *     transliteration of upstream's `apc.zig` `.kitty` prong plus
 *     upstream's `stream_terminal.zig` `apcEnd` handler, so the behavior
 *     asserted here is upstream's.
 *
 *   - The Sixel DCS path, `ESC P P1;P2;P3 q <body> ESC \`. Wisp-specific:
 *     Ghostty does not implement Sixel at all (its only mention upstream is
 *     `sixel = 4` in device_attributes.zig), so there is no upstream test
 *     to port and these assert the DEC format's own behavior as it reaches
 *     the decoder and the image store.
 *
 * Wisp, the test-harness seams, as in the sibling port test files:
 *   - `test_helpers.h`'s `TEST(suite, name)` takes a C identifier.
 *   - `test_helpers.h`'s `ASSERT_EQ` casts both sides to `long`, which
 *     truncates 64-bit values and pointers, so those are compared with
 *     `ASSERT_TRUE(a == b)`.
 *   - `ASSERT_STR_EQ` expands to several statements, so a temporary
 *     `std::string` argument would dangle; string comparisons go through
 *     `EXPECT_STR`, which binds a named local first.
 *   - Escapes are written `"\x1b" "_G"` because C++ hex escapes are greedy.
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/stream_terminal.hpp"
#include "../terminal/sixel.hpp"

using namespace wisp;

namespace st = wisp::terminal::stream_terminal;
namespace kgfx = wisp::terminal::kitty::graphics;
namespace kstore = wisp::terminal::kitty::graphics_storage;
namespace kimage = wisp::terminal::kitty::graphics_image;
namespace sx = wisp::terminal::sixel;

typedef st::Handler Handler;
typedef wisp::terminal::stream::Stream<Handler> Stream;
typedef kstore::ImageStorage ImageStorage;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `var t: Terminal = try .init(...); defer t.deinit(alloc);` */
struct TermHolder {
    vt::Terminal t;
    bool ok;
    TermHolder(unsigned cols, unsigned rows) {
        vt::Terminal::Options o((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
        ok = vt::Terminal::init(talloc(), o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

#define TERM(v, c, r)                                                                              \
    TermHolder v##_holder((c), (r));                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    vt::Terminal &v = v##_holder.t

/* Wisp: the stream is not copyable, so it is constructed in place around
 * the handler. */
struct StreamHolder {
    Stream s;
    explicit StreamHolder(const Handler &h) : s(h, streamOptions()) {}
    ~StreamHolder() { s.deinit(); }

    static Stream::Options streamOptions() {
        Stream::Options o;
        o.allocator = true;
        return o;
    }
};

#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

/* ── pty capture ───────────────────────────────────────────────────────── */

static char g_pty[4096];
static size_t g_pty_len = 0;
static size_t g_pty_calls = 0;

static void writePtyFn(Handler *, const char *data, size_t len) {
    if (g_pty_len + len <= sizeof g_pty) {
        memcpy(g_pty + g_pty_len, data, len);
        g_pty_len += len;
    }
    g_pty_calls += 1;
}

static std::string ptyResponse() { return std::string(g_pty, g_pty_len); }

static void resetPty() {
    g_pty_len = 0;
    g_pty_calls = 0;
}

/* ── image helpers ─────────────────────────────────────────────────────── */

static ImageStorage *storageOf(vt::Terminal *t) { return &t->screens.active->kitty_images; }

/* The first image in storage, if any. The assertion macros in
 * test_helpers.h expand to `return;`, so they cannot live in a function
 * that returns a value; ONLY_IMAGE below does the asserting. */
static bool firstImage(ImageStorage *storage, kimage::Image *out) {
    ImageStorage::ImageMap::Iterator it = storage->images.iterator();
    ImageStorage::ImageMap::Entry entry;
    if (!it.next(&entry)) return false;
    *out = *entry.value_ptr;
    return true;
}

/* Binds `v` to the single image in `storage`, asserting there is exactly
 * one. */
#define ONLY_IMAGE(v, storage)                                                                         kimage::Image v;                                                                                   do {                                                                                                   ImageStorage *_s = (storage);                                                                       ASSERT_EQ(1, (long)_s->images.count());                                                            ASSERT_TRUE(firstImage(_s, &v));                                                               } while (0)

static std::string imageBytes(const kimage::Image &img) {
    const uint8_t *p = nullptr;
    size_t n = 0;
    if (!img.data.bytes(&p, &n)) return std::string();
    return std::string((const char *)p, n);
}

/* ── Part 1: the APC Kitty path ────────────────────────────────────────── */

/* A full transmit-and-display reaches graphics_exec, leaves exactly one
 * image with the transmitted dimensions and bytes, and emits the OK
 * response for the client-chosen id. */
TEST(graphics_wiring, kitty_apc_transmits_and_responds) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    /* f=24 is RGB, so s=2,v=1 is 6 payload bytes: 01 02 03 04 05 06. */
    sh.s.nextSlice("\x1b" "_Ga=T,f=24,s=2,v=1,i=7;AQIDBAUG\x1b" "\\");

    ImageStorage *storage = storageOf(&t);
    ONLY_IMAGE(img, storage);
    ASSERT_EQ(7, (long)img.id);
    ASSERT_EQ(2, (long)img.width);
    ASSERT_EQ(1, (long)img.height);
    ASSERT_TRUE(img.format == kgfx::Transmission::Format::rgb);
    ASSERT_TRUE(img.compression == kgfx::Transmission::Compression::none);

    static const char want[6] = {1, 2, 3, 4, 5, 6};
    const std::string got = imageBytes(img);
    ASSERT_EQ(6, (long)got.size());
    ASSERT_TRUE(0 == memcmp(got.data(), want, 6));

    /* a=T also places it, at the cursor. */
    ASSERT_EQ(1, (long)storage->placements.count());

    EXPECT_STR("\x1b" "_Gi=7;OK\x1b" "\\", ptyResponse());
}

/* A plain transmit (a=t) with no display still stores the image and still
 * answers, so the response is not a side effect of placement. */
TEST(graphics_wiring, kitty_apc_transmit_only_responds) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "_Ga=t,f=32,s=2,v=1,i=9;AQIDBAUGBwg=\x1b" "\\");

    ImageStorage *storage = storageOf(&t);
    ONLY_IMAGE(img, storage);
    ASSERT_EQ(9, (long)img.id);
    ASSERT_EQ(2, (long)img.width);
    ASSERT_EQ(1, (long)img.height);
    ASSERT_TRUE(img.format == kgfx::Transmission::Format::rgba);
    ASSERT_EQ(8, (long)imageBytes(img).size());
    ASSERT_EQ(0, (long)storage->placements.count());

    EXPECT_STR("\x1b" "_Gi=9;OK\x1b" "\\", ptyResponse());
}

/* q=1 suppresses the OK. The suppression lives inside graphics_exec, which
 * returns no response at all, so this asserts that the wiring does not
 * manufacture one. */
TEST(graphics_wiring, kitty_apc_quiet_emits_no_response) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "_Ga=t,f=24,s=2,v=1,i=7,q=1;AQIDBAUG\x1b" "\\");

    /* The command still took effect. */
    ImageStorage *storage = storageOf(&t);
    ONLY_IMAGE(img, storage);
    ASSERT_EQ(7, (long)img.id);

    ASSERT_EQ(0, (long)g_pty_calls);
    ASSERT_EQ(0, (long)g_pty_len);
}

/* q=2 suppresses failures as well as successes: an error response must not
 * reach the pty either. */
TEST(graphics_wiring, kitty_apc_quiet_failures_emits_no_response) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    /* i= and I= together is EINVAL. */
    sh.s.nextSlice("\x1b" "_Ga=t,f=24,s=2,v=1,i=7,I=8,q=2;AQIDBAUG\x1b" "\\");

    ASSERT_EQ(0, (long)g_pty_calls);
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());
}

/* A malformed command errors in the parser, which drops to the ignore state.
 * Nothing is stored, nothing is written, the screen is untouched, and the
 * parser's buffer is freed (the trailing leak check covers that). */
TEST(graphics_wiring, kitty_apc_malformed_command_is_dropped) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    /* Print a known cell first so "the screen is untouched" has something
     * to be true of. */
    sh.s.nextSlice("A");

    /* i=10000000000 overflows u32 during accumulation, which is an error
     * mid-feed: the parser is deinited and the rest of the APC dropped. */
    sh.s.nextSlice("\x1b" "_Ga=p,i=10000000000;AQIDBAUG\x1b" "\\");

    /* A second, differently malformed one: garbage immediately after G,
     * which fails at `complete` rather than during feed. */
    sh.s.nextSlice("\x1b" "_Gabcdef1234\x1b" "\\");

    /* And one whose payload is not valid base64. */
    sh.s.nextSlice("\x1b" "_Ga=t,f=24,s=1,v=1,i=3;!!!!\x1b" "\\");

    ASSERT_EQ(0, (long)storageOf(&t)->images.count());
    ASSERT_EQ(0, (long)storageOf(&t)->placements.count());
    ASSERT_EQ(0, (long)g_pty_calls);

    /* The cell written before the malformed sequences is still there and
     * the cursor is still just past it: nothing walked the screen. */
    ASSERT_EQ(1, (long)t.screens.active->cursor.x);
    ASSERT_EQ(0, (long)t.screens.active->cursor.y);

    /* And normal printing still works afterwards. */
    sh.s.nextSlice("B");
    ASSERT_EQ(2, (long)t.screens.active->cursor.x);
}

/* With no write_pty effect the command still executes; only the response is
 * skipped. This is the path upstream guards with
 * `if (self.effects.write_pty == null) break :resp`. */
TEST(graphics_wiring, kitty_apc_without_write_pty_still_executes) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    /* effects.write_pty deliberately left null. */
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "_Ga=t,f=24,s=2,v=1,i=7;AQIDBAUG\x1b" "\\");

    ASSERT_EQ(1, (long)storageOf(&t)->images.count());
    ASSERT_EQ(0, (long)g_pty_calls);
}

/* The payload may be split across stream slices, which is the realistic
 * case for a large image: the parser is fed through apc_put_slice. */
TEST(graphics_wiring, kitty_apc_split_across_slices) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "_Ga=t,f=24,");
    sh.s.nextSlice("s=2,v=1,i=7;AQ");
    sh.s.nextSlice("IDBAUG\x1b" "\\");

    ONLY_IMAGE(img, storageOf(&t));
    ASSERT_EQ(7, (long)img.id);
    static const char want[6] = {1, 2, 3, 4, 5, 6};
    const std::string got = imageBytes(img);
    ASSERT_EQ(6, (long)got.size());
    ASSERT_TRUE(0 == memcmp(got.data(), want, 6));
    EXPECT_STR("\x1b" "_Gi=7;OK\x1b" "\\", ptyResponse());
}

/* ── Part 2: the Sixel DCS path ────────────────────────────────────────── */

/* Decode a body directly, for use as the reference an end-to-end sixel test
 * compares against. */
struct RefImage {
    sx::Image img;
    RefImage(const char *body, sx::BackgroundSelect bg) {
        img.pixels = nullptr;
        img.width = 0;
        img.height = 0;
        sx::Options o = sx::Options::defaults();
        o.background_select = bg;
        (void)sx::decode(talloc(), body, strlen(body), o, &img);
    }
    ~RefImage() { img.deinit(talloc()); }
};

/* `ESC P q <body> ESC \` stores an image whose pixels are exactly what
 * sixel::decode produces for the same body. */
TEST(graphics_wiring, sixel_dcs_stores_decoded_image) {
    TERM(t, 10, 5);
    resetPty();

    static const char *const body = "#1@A#2BC";

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "Pq#1@A#2BC\x1b" "\\");

    ImageStorage *storage = storageOf(&t);
    ONLY_IMAGE(img, storage);

    /* Decoded Sixel output is RGBA8888 and is stored uncompressed. */
    ASSERT_TRUE(img.format == kgfx::Transmission::Format::rgba);
    ASSERT_TRUE(img.compression == kgfx::Transmission::Compression::none);

    const RefImage ref(body, sx::BackgroundSelect::paint);
    ASSERT_TRUE(ref.img.pixels != nullptr);
    ASSERT_TRUE((size_t)img.width == ref.img.width);
    ASSERT_TRUE((size_t)img.height == ref.img.height);

    const std::string got = imageBytes(img);
    const size_t want_len = ref.img.width * ref.img.height * 4;
    ASSERT_TRUE(got.size() == want_len);
    ASSERT_TRUE(0 == memcmp(got.data(), ref.img.pixels, want_len));

    /* It is placed at the cursor, in the same store, through the same
     * placement machinery a Kitty image uses. */
    ASSERT_EQ(1, (long)storage->placements.count());

    /* The id came from the storage's implicit allocation, not from the
     * body, so it cannot collide with a client-chosen Kitty id: it is in
     * the upper half of the range and marked unaddressable. */
    ASSERT_TRUE(img.id >= 2147483647u);
    ASSERT_TRUE(img.metadata.implicit_id);

    /* Sixel has no response of any kind. */
    ASSERT_EQ(0, (long)g_pty_calls);
}

/* The implicit id does not take an id a Kitty client already owns, and a
 * Kitty client transmitting an explicit id does not take the Sixel image's.
 * Two Sixel images get two distinct ids. */
TEST(graphics_wiring, sixel_dcs_id_does_not_collide_with_kitty) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "_Ga=t,f=24,s=2,v=1,i=7;AQIDBAUG\x1b" "\\");
    sh.s.nextSlice("\x1b" "Pq#1@\x1b" "\\");
    sh.s.nextSlice("\x1b" "Pq#2@\x1b" "\\");

    ImageStorage *storage = storageOf(&t);
    ASSERT_EQ(3, (long)storage->images.count());
    ASSERT_TRUE(storage->imageById(7).has);

    /* Collect the two non-Kitty ids and check they differ and avoid 7. */
    uint32_t ids[2] = {0, 0};
    size_t n = 0;
    ImageStorage::ImageMap::Iterator it = storage->images.iterator();
    ImageStorage::ImageMap::Entry entry;
    while (it.next(&entry)) {
        if (entry.value_ptr->id == 7) continue;
        ASSERT_TRUE(n < 2);
        ids[n] = entry.value_ptr->id;
        n += 1;
    }
    ASSERT_EQ(2, (long)n);
    ASSERT_TRUE(ids[0] != ids[1]);
    ASSERT_TRUE(ids[0] != 7u && ids[1] != 7u);
}

/* P2 = 1 reaches the decoder as `transparent`: a zero bit leaves the pixel
 * alone, so only the row the set bit names exists. */
TEST(graphics_wiring, sixel_dcs_p2_1_is_transparent) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "P0;1;0q#1@\x1b" "\\");

    ONLY_IMAGE(img, storageOf(&t));
    ASSERT_EQ(1, (long)img.width);
    ASSERT_EQ(1, (long)img.height);

    const RefImage ref("#1@", sx::BackgroundSelect::transparent);
    ASSERT_TRUE(1 == ref.img.height);
    const std::string got = imageBytes(img);
    ASSERT_TRUE(got.size() == 4);
    ASSERT_TRUE(0 == memcmp(got.data(), ref.img.pixels, 4));
}

/* P2 = 0 reaches the decoder as `paint`: the zero bits of the same byte
 * paint the background, so the full six-row band exists. */
TEST(graphics_wiring, sixel_dcs_p2_0_is_paint) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "P0;0;0q#1@\x1b" "\\");

    ONLY_IMAGE(img, storageOf(&t));
    ASSERT_EQ(1, (long)img.width);
    ASSERT_EQ(6, (long)img.height);

    const RefImage ref("#1@", sx::BackgroundSelect::paint);
    ASSERT_TRUE(6 == ref.img.height);
    const std::string got = imageBytes(img);
    ASSERT_TRUE(got.size() == 24);
    ASSERT_TRUE(0 == memcmp(got.data(), ref.img.pixels, 24));
}

/* P2 = 2 is the other "paint the background" value, and an absent P2 is the
 * default, which is also paint. */
TEST(graphics_wiring, sixel_dcs_p2_2_and_absent_are_paint) {
    {
        TERM(t, 10, 5);
        Handler handler = Handler::init(&t);
        StreamHolder sh(handler);
        sh.s.nextSlice("\x1b" "P0;2;0q#1@\x1b" "\\");
        ONLY_IMAGE(img, storageOf(&t));
        ASSERT_EQ(6, (long)img.height);
    }
    {
        TERM(t, 10, 5);
        Handler handler = Handler::init(&t);
        StreamHolder sh(handler);
        sh.s.nextSlice("\x1b" "Pq#1@\x1b" "\\");
        ONLY_IMAGE(img, storageOf(&t));
        ASSERT_EQ(6, (long)img.height);
    }
}

/* A body with no data bytes decodes to an empty image, which there is
 * nothing to store. */
TEST(graphics_wiring, sixel_dcs_empty_body_stores_nothing) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    StreamHolder sh(handler);

    sh.s.nextSlice("\x1b" "Pq\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    /* Colour registers but no data bytes: still nothing drawn. */
    sh.s.nextSlice("\x1b" "P0;1;0q#1;2;100;0;0#1\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());
    ASSERT_EQ(0, (long)storageOf(&t)->placements.count());
}

/* Hostile bodies are refused by the decoder's size limits, which are left
 * at their defaults on purpose. Nothing is stored, nothing is placed, and
 * nothing is leaked (the trailing leak check covers that). The sequences
 * below are the same shapes test_sixel.cpp asserts TooLarge on, here driven
 * through the pty path instead of through `decode` directly. */
TEST(graphics_wiring, sixel_dcs_hostile_body_stores_nothing) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    handler.effects.write_pty = &writePtyFn;
    StreamHolder sh(handler);

    /* Print a cell first so the screen has known contents. */
    sh.s.nextSlice("A");

    /* A raster attribute asking for ~10^10 pixels. */
    sh.s.nextSlice("\x1b" "Pq\"1;1;99999;99999#1@\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    /* A repeat introducer asking for a billion columns. */
    sh.s.nextSlice("\x1b" "Pq#1!999999999~\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    /* A repeat count that would overflow a u32 if accumulated naively. */
    sh.s.nextSlice("\x1b" "Pq#1!99999999999999999999~\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    /* Both at once, plus an out-of-range colour register. */
    sh.s.nextSlice("\x1b" "Pq\"1;1;60000;60000#99999!4294967296~\x1b" "\\");
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    ASSERT_EQ(0, (long)storageOf(&t)->placements.count());
    ASSERT_EQ(0, (long)g_pty_calls);

    /* The screen survived and still prints. */
    ASSERT_EQ(1, (long)t.screens.active->cursor.x);
    sh.s.nextSlice("B");
    ASSERT_EQ(2, (long)t.screens.active->cursor.x);

    /* And a legitimate body right afterwards still works, so the handler
     * recovered rather than wedging. */
    sh.s.nextSlice("\x1b" "Pq#1@\x1b" "\\");
    ASSERT_EQ(1, (long)storageOf(&t)->images.count());
}

/* The DCS body is bounded by the DCS handler's own max_bytes, so an
 * unterminated Sixel cannot buffer without limit. Exceeding it discards the
 * sequence the same way an over-long XTGETTCAP does. */
TEST(graphics_wiring, sixel_dcs_body_is_bounded) {
    TERM(t, 10, 5);
    resetPty();

    Handler handler = Handler::init(&t);
    StreamHolder sh(handler);

    /* Shrink the bound so the test does not have to feed a megabyte. */
    sh.s.handler.dcs_handler.max_bytes = 8;

    sh.s.nextSlice("\x1b" "Pq");
    sh.s.nextSlice("~~~~~~~~");         /* 8 bytes: at the limit */
    sh.s.nextSlice("~~~~~~~~");         /* over it: discarded */
    sh.s.nextSlice("\x1b" "\\");

    /* The whole sequence was dropped on the overflow, so no image. */
    ASSERT_EQ(0, (long)storageOf(&t)->images.count());

    /* A later, in-bounds sequence still works. */
    sh.s.nextSlice("\x1b" "Pq#1@\x1b" "\\");
    ASSERT_EQ(1, (long)storageOf(&t)->images.count());
}

/* Sixel must not be advertised in Device Attributes until images actually
 * render: a terminal that claims the capability and then swallows images is
 * worse than one that does not claim it. */
TEST(graphics_wiring, sixel_is_not_advertised_in_device_attributes) {
    typedef wisp::terminal::device_attributes::Primary Primary;
    const Primary p;
    for (size_t i = 0; i < p.features_len; i++) {
        ASSERT_TRUE(p.features[i] != Primary::Feature::sixel);
    }
    std::string w;
    p.encode(&w);
    ASSERT_TRUE(strstr(w.c_str(), ";4c") == nullptr);
    ASSERT_TRUE(strstr(w.c_str(), ";4;") == nullptr);
}

TEST(graphics_wiring, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
