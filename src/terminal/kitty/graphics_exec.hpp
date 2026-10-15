/* Transliterated from Ghostty src/terminal/kitty/graphics_exec.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *
 *   - This file is ported in full, with every test. `kitty_graphics` was
 *     false when the surrounding subsystem was ported and this file was
 *     omitted; it is in scope now. Nothing here is gated on
 *     `glyph_protocol` or `tmux_control_mode`, so nothing is omitted for
 *     those flags.
 *
 *   - `std.Io` is not carried over (see screen.hpp), so the `io`
 *     parameter is dropped from every signature here and from every
 *     `ImageStorage` call (see graphics_storage.hpp).
 *
 *   - `?Response` is a `bool` return plus an out parameter on the public
 *     `execute`; the file-local helpers return `Response` by value, as
 *     upstream does (a `Response` owns nothing — its `message` is a
 *     static string).
 *
 *   - `Image.Error || Allocator.Error` is `graphics_image::Error`
 *     (`none` on success), which already carries `OutOfMemory`.
 *     `loadAndAddImage`'s `!struct { image, more, display }` is that
 *     error return plus the `Load` out parameter below, whose
 *     `?command.Display display` is a `has_display` flag beside the
 *     field (the same shape `LoadingImage` uses).
 *
 *   - `?T` elsewhere is `Maybe<T>` (`resolveParent`'s `child`) or a
 *     plain nullable pointer where the payload is already one
 *     (`storage.loading`, `img.animation`, `ensureAnimation`'s
 *     `Allocator.Error!*Animation`).
 *
 *   - `union(enum)` payload access is tag-plus-field:
 *     `cmd.control.transmit_animation_frame`,
 *     `p.location.relative`, `Location::makePin/makeVirtual/makeRelative`.
 *     `.virtual` is spelled `virtual_` and `ImageStorage.delete` is
 *     `delete_`; `virtual` and `delete` are C++ keywords.
 *
 *   - `labeled block` + `break :label value` is a local plus straight-line
 *     branches: `transmit`'s `resp:`, `display`'s `location:` and
 *     `placement:`, `completeAnimationFrame`'s `number:`,
 *     `loadAndAddImage`'s `loading:`.
 *
 *   - `defer` / `errdefer` are explicit cleanup at each exit path.
 *     `transmit`'s `errdefer load.image.deinit(alloc)` has no counterpart
 *     because `transmit` returns a `Response`, not an error union, so
 *     that errdefer can never fire upstream either.
 *
 *   - `log.debug` / `log.warn` have no sink in the port and are comments.
 *
 *   - `@import("../../quirks.zig").inlineAssert` is plain `assert`.
 *
 *   - `+|` / `-|` saturating operators are graphics_storage.hpp's `sat*`
 *     helpers. The cursor-movement arithmetic upstream does in `usize`
 *     is done in `uint64_t` with `satAddU64`.
 *
 *   - Two places copy a heap `LoadingImage` out and free the cell
 *     without deinit (upstream's `loading.*` plus `alloc.destroy`).
 *     The copy is a move-out: the heap cell is released without
 *     `deinit`, so the owned `data` list has exactly one owner at all
 *     times and nothing is double-freed. The reverse direction
 *     (publishing a stack `LoadingImage` to the heap) is a placement-new
 *     copy into the raw cell `Allocator::create` hands back, rather
 *     than an assignment into unconstructed memory.
 *
 *   - `for (0..rows_to_move)` is a C++ `for` over a `const` bound
 *     snapshotted before the loop; the body calls `terminal->index()`,
 *     which mutates the screen.
 *
 * Wisp, the one behavioral note:
 *
 *   - `encodeError` switches over upstream's `Image.Error ||
 *     Allocator.Error`, which is exactly 12 values. graphics_image.hpp's
 *     `Error` additionally carries `UncPath`, `DevicePath`,
 *     `ReservedDeviceName` and `NotDriveAbsolute` (see its note: they
 *     surface out of `validatedFilePath` and every caller in that file
 *     maps them to `InvalidData` before they can reach here). They
 *     cannot reach `encodeError`, but since C++ cannot prove that, they
 *     and `none` map to the same message `InvalidData` gets, rather than
 *     leaving `message` as the "OK" default.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_EXEC_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_EXEC_HPP

#include <assert.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../vt/screen.hpp"
#include "../../vt/size.hpp"
#include "../../vt/terminal.hpp"
#include "../../zigstd/allocator.hpp"
#include "graphics_animation.hpp"
#include "graphics_command.hpp"
#include "graphics_image.hpp"
#include "graphics_pixel.hpp"
#include "graphics_storage.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_exec {

typedef ::wisp::zigstd::Allocator Allocator;

namespace animation = ::wisp::terminal::kitty::graphics_animation;
namespace command = ::wisp::terminal::kitty::graphics;
namespace image = ::wisp::terminal::kitty::graphics_image;
namespace pixel = ::wisp::terminal::kitty::graphics_pixel;

typedef ::wisp::vt::Terminal Terminal;
typedef ::wisp::vt::Screen Screen;
typedef ::wisp::vt::PageList PageList;
typedef ::wisp::terminal::kitty::graphics::Command Command;
typedef ::wisp::terminal::kitty::graphics::Response Response;
typedef ::wisp::terminal::kitty::graphics_image::LoadingImage LoadingImage;
typedef ::wisp::terminal::kitty::graphics_image::Image Image;
typedef ::wisp::terminal::kitty::graphics_storage::ImageStorage ImageStorage;

using ::wisp::vt::Maybe;

/* Wisp: upstream's `EncodeableError`, which is `Image.Error ||
 * Allocator.Error`. graphics_image.hpp already folds `Allocator.Error`
 * into its `Error` as `OutOfMemory`. */
typedef ::wisp::terminal::kitty::graphics_image::Error EncodeableError;

/* Wisp: forward declarations; upstream's Zig needs none. Declaration
 * order below is upstream's. */
static Response query(Allocator alloc, const Terminal *terminal, const Command *cmd);
static Response transmit(Allocator alloc, Terminal *terminal, const Command *cmd);
static Response display(Allocator alloc, Terminal *terminal, const Command *cmd);
static Response transmitAnimationFrame(Allocator alloc, Terminal *terminal, const Command *cmd);
static Response completeAnimationFrame(Allocator alloc, Terminal *terminal,
                                       LoadingImage *loading);
static Response controlAnimation(Allocator alloc, Terminal *terminal, const Command *cmd);
static Response composeAnimation(Allocator alloc, Terminal *terminal, const Command *cmd);
static animation::Animation *ensureAnimation(Allocator alloc, Image *img);
static Response delete_(Allocator alloc, Terminal *terminal, const Command *cmd);
static void encodeError(Response *r, EncodeableError err);

/* Wisp: `loadAndAddImage`'s anonymous result struct. */
struct Load {
    Image image;
    bool more;              /* = false */
    bool has_display;       /* = false (null) */
    command::Display display;

    Load() : image(), more(false), has_display(false), display() {}
};

static EncodeableError loadAndAddImage(Allocator alloc, Terminal *terminal, const Command *cmd,
                                       Load *out);

/// Execute a Kitty graphics command against the given terminal. This
/// will never fail, but the response may indicate an error and the
/// terminal state may not be updated to reflect the command. This will
/// never put the terminal in an unrecoverable state, however.
///
/// The allocator must be the same allocator that was used to build
/// the command.
///
/// Wisp: `?Response` is a bool return plus an out parameter.
inline bool execute(Allocator alloc, Terminal *terminal, const Command *cmd, Response *out) {
    /* If storage is disabled then we disable the full protocol. This means
     * we don't even respond to queries so the terminal completely acts as
     * if this feature is not supported. */
    if (!terminal->screens.active->kitty_images.enabled()) {
        /* log.debug("kitty graphics requested but disabled", .{}) */
        return false;
    }

    /* log.debug("executing kitty graphics command: quiet={} control={}") */

    /* The quiet settings used to control the response. We have to make this
     * a var because in certain special cases (namely chunked transmissions)
     * this can change. */
    Command::Quiet quiet = cmd->quiet;

    /* The protocol makes i and I mutually exclusive for every action, so this
     * must happen before dispatch and before an action can mutate storage.
     * https://sw.kovidgoyal.net/kitty/graphics-protocol/#requesting-image-ids-from-the-terminal */
    const Command::Control::Identifiers identifiers = cmd->control.identifiers();
    if (identifiers.image_id > 0 && identifiers.image_number > 0) {
        Response resp;
        resp.id = identifiers.image_id;
        resp.image_number = identifiers.image_number;
        resp.placement_id = identifiers.placement_id;
        resp.message = "EINVAL: image ID and number are mutually exclusive";
        resp.message_len = strlen(resp.message);
        /* log.warn("erroneous kitty graphics response: {s}", .{resp.message}) */

        switch (quiet) {
        case Command::Quiet::no: *out = resp; return true;
        case Command::Quiet::ok: *out = resp; return true;
        case Command::Quiet::failures: return false;
        }
        return false;
    }

    /* Wisp: upstream's `const resp_: ?Response = switch (cmd.control)`. */
    bool have_resp = false;
    Response resp;

    switch (cmd->control.tag) {
    case Command::Action::query:
        resp = query(alloc, terminal, cmd);
        have_resp = true;
        break;
    case Command::Action::display:
        resp = display(alloc, terminal, cmd);
        have_resp = true;
        break;
    case Command::Action::delete_:
        resp = delete_(alloc, terminal, cmd);
        have_resp = true;
        break;

    case Command::Action::transmit:
    case Command::Action::transmit_and_display:
    case Command::Action::transmit_animation_frame: {
        /* If we're transmitting, then our `q` setting value is complicated.
         * The `q` setting inherits the value from the starting command
         * unless `q` is set >= 1 on this command. If it is, then we save
         * that as the new `q` setting. */
        ImageStorage *storage = &terminal->screens.active->kitty_images;
        if (storage->loading != nullptr) {
            LoadingImage *loading = storage->loading;
            switch (cmd->quiet) {
            /* q=0 we use whatever the start command value is */
            case Command::Quiet::no: quiet = loading->quiet; break;

            /* q>=1 we use the new value, but we should already be set to it */
            case Command::Quiet::ok:
                assert(quiet == Command::Quiet::ok);
                loading->quiet = Command::Quiet::ok;
                break;
            case Command::Quiet::failures:
                assert(quiet == Command::Quiet::failures);
                loading->quiet = Command::Quiet::failures;
                break;
            }
        }

        if (cmd->control.tag == Command::Action::transmit_animation_frame) {
            resp = transmitAnimationFrame(alloc, terminal, cmd);
        } else {
            resp = transmit(alloc, terminal, cmd);
        }
        have_resp = true;
        break;
    }

    case Command::Action::control_animation:
        resp = controlAnimation(alloc, terminal, cmd);
        have_resp = true;
        break;

    case Command::Action::compose_animation:
        resp = composeAnimation(alloc, terminal, cmd);
        have_resp = true;
        break;
    }

    /* Handle the quiet settings */
    if (have_resp) {
        if (!resp.ok()) {
            /* log.warn("erroneous kitty graphics response: {s}", .{resp.message}) */
        }

        switch (quiet) {
        case Command::Quiet::no:
            if (resp.empty()) return false;
            *out = resp;
            return true;
        case Command::Quiet::ok:
            if (resp.ok()) return false;
            *out = resp;
            return true;
        case Command::Quiet::failures: return false;
        }
    }

    return false;
}

/// Execute a "query" command.
///
/// This command is used to attempt to load an image and respond with
/// success/error but does not persist any of the command to the terminal
/// state.
static Response query(Allocator alloc, const Terminal *terminal, const Command *cmd) {
    const command::Transmission t = cmd->control.query;

    /* Query requires image ID. We can't actually send a response without
     * an image ID either but we return an error and this will be logged
     * downstream. */
    if (t.image_id == 0) {
        Response r;
        r.message = "EINVAL: image ID required";
        r.message_len = strlen(r.message);
        return r;
    }

    /* Build a partial response to start */
    Response result;
    result.id = t.image_id;
    result.image_number = t.image_number;
    result.placement_id = t.placement_id;

    /* A query must attempt a complete load, then discard the result without
     * changing image storage.
     * https://sw.kovidgoyal.net/kitty/graphics-protocol/#querying-support-and-available-transmission-mediums */
    const ImageStorage *storage = &terminal->screens.active->kitty_images;
    LoadingImage loading;
    {
        const EncodeableError err =
            LoadingImage::init(alloc, cmd, storage->image_limits, &loading);
        if (err != EncodeableError::none) {
            encodeError(&result, err);
            return result;
        }
    }
    /* defer loading.deinit(alloc); */

    Image img;
    {
        const EncodeableError err = loading.complete(alloc, &img);
        if (err != EncodeableError::none) {
            encodeError(&result, err);
            loading.deinit(alloc);
            return result;
        }
    }
    img.deinit(alloc);

    loading.deinit(alloc);
    return result;
}

/// Transmit image data.
///
/// This loads the image, validates it, and puts it into the terminal
/// screen storage. It does not display the image.
static Response transmit(Allocator alloc, Terminal *terminal, const Command *cmd) {
    command::Transmission t;
    {
        const bool have = cmd->transmission(&t);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    Response result;
    if (storage->loading != nullptr) {
        LoadingImage *loading = storage->loading;
        /* Any transmit-like command received while a load is in progress
         * is a continuation chunk of that load, and the load completes
         * according to how it started. If an animation frame load (a=f)
         * is in progress, this chunk belongs to it even when the client
         * didn't repeat a=f on the chunk. */
        if (loading->has_frame) return transmitAnimationFrame(alloc, terminal, cmd);

        result = loading->response;
    } else {
        result.id = t.image_id;
        result.image_number = t.image_number;
        result.placement_id = t.placement_id;
    }

    Load load;
    {
        const EncodeableError err = loadAndAddImage(alloc, terminal, cmd, &load);
        if (err != EncodeableError::none) {
            encodeError(&result, err);
            return result;
        }
    }
    /* Wisp: upstream's `errdefer load.image.deinit(alloc)` can never fire;
     * this function returns a Response, not an error union. */

    /* If we're also displaying, then do that now. This function does
     * both transmit and transmit and display. The display might also be
     * deferred if it is multi-chunk. */
    if (load.has_display) {
        assert(!load.more);
        command::Display d_copy = load.display;
        d_copy.image_id = load.image.id;
        Command display_cmd;
        display_cmd.control.tag = Command::Action::display;
        display_cmd.control.display = d_copy;
        display_cmd.quiet = cmd->quiet;
        result = display(alloc, terminal, &display_cmd);
    }

    /* If there are more chunks expected we do not respond. */
    if (load.more) return Response();

    /* If the loaded image was assigned its ID automatically, not based
     * on a number or explicitly specified ID, then we don't respond. */
    if (load.image.metadata.implicit_id) return Response();

    /* After the image is added, set the ID in case it changed.
     * The resulting image number and placement ID never change. */
    result.id = load.image.id;

    return result;
}

/// Display a previously transmitted image.
static Response display(Allocator alloc, Terminal *terminal, const Command *cmd) {
    command::Display d;
    {
        const bool have = cmd->display(&d);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }

    /* Display requires image ID or number. */
    if (d.image_id == 0 && d.image_number == 0) {
        Response r;
        r.message = "EINVAL: image ID or number required";
        r.message_len = strlen(r.message);
        return r;
    }

    /* Build up our response */
    Response result;
    result.id = d.image_id;
    result.image_number = d.image_number;
    result.placement_id = d.placement_id;

    /* A virtual placement (U=1) cannot also be a relative placement.
     * Kitty checks this before even looking up the image. */
    if (d.virtual_placement && d.parent_id > 0) {
        result.message = "EINVAL: virtual placement cannot refer to a parent";
        result.message_len = strlen(result.message);
        return result;
    }

    /* Verify the requested image exists if we have an ID */
    ImageStorage *storage = &terminal->screens.active->kitty_images;
    const Maybe<Image> img_ =
        d.image_id != 0 ? storage->imageById(d.image_id) : storage->imageByNumber(d.image_number);
    if (!img_.has) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }
    const Image img = img_.value;

    /* Make sure our response has the image id in case we looked up by number */
    result.id = img.id;

    /* Location where the placement will go.
     * Wisp: upstream's `location:` labeled block. */
    ImageStorage::Placement::Location location;
    {
        /* Virtual placements are not tracked */
        if (d.virtual_placement) {
            location = ImageStorage::Placement::Location::makeVirtual();
        } else if (d.parent_id == 0) {
            /* No parent reference (P=): the placement is pinned to the
             * cursor. The cursor is always tracked but we don't want
             * this pin to move with the cursor. */
            PageList::Pin *pin =
                terminal->screens.active->pages.trackPin(*terminal->screens.active->cursor.page_pin);
            if (pin == nullptr) {
                /* log.warn("failed to create pin for Kitty graphics err={}") */
                result.message = "EINVAL: failed to prepare terminal state";
                result.message_len = strlen(result.message);
                return result;
            }
            location = ImageStorage::Placement::Location::makePin(pin);
        } else {
            /* A parent reference makes this a relative placement: it is
             * positioned relative to the parent placement instead of the
             * cursor. */

            /* The key of the placement being created, when it is
             * addressable (an explicit placement ID). Needed for
             * self-parent and cycle detection. */
            Maybe<ImageStorage::PlacementKey> child =
                Maybe<ImageStorage::PlacementKey>::none();
            if (d.placement_id > 0) {
                child = Maybe<ImageStorage::PlacementKey>(ImageStorage::PlacementKey(
                    img.id, ImageStorage::PlacementId::external(d.placement_id)));
            }

            ImageStorage::PlacementKey parent;
            const ImageStorage::ParentError err =
                storage->resolveParent(terminal->screens.active, child, d.parent_id,
                                       d.parent_placement_id, &parent);
            if (err != ImageStorage::ParentError::none) {
                switch (err) {
                case ImageStorage::ParentError::ParentImageNotFound:
                    result.message = "ENOPARENT: parent image not found";
                    break;
                case ImageStorage::ParentError::ParentPlacementNotFound:
                    result.message = "ENOPARENT: parent placement not found";
                    break;
                case ImageStorage::ParentError::SelfParent:
                    result.message = "EINVAL: placement cannot be its own parent";
                    break;
                case ImageStorage::ParentError::Cycle:
                    result.message = "ECYCLE: parent chain creates a cycle";
                    break;
                case ImageStorage::ParentError::TooDeep:
                    result.message = "ETOODEEP: parent chain too deep";
                    break;
                case ImageStorage::ParentError::AncestorNotFound:
                    result.message = "ENOENT: parent chain ancestor not found";
                    break;
                /* Wisp: `none` is the success path, excluded by the `if`. */
                case ImageStorage::ParentError::none: assert(false); break;
                }
                result.message_len = strlen(result.message);
                return result;
            }

            ImageStorage::Placement::Relative rel;
            rel.parent = parent;
            rel.horizontal_offset = d.horizontal_offset;
            rel.vertical_offset = d.vertical_offset;
            location = ImageStorage::Placement::Location::makeRelative(rel);
        }
    }

    /* Add the placement
     * Wisp: upstream's `placement:` labeled block. */
    ImageStorage::Placement p;
    {
        p.location = location;
        p.x_offset = d.x_offset;
        p.y_offset = d.y_offset;
        p.source_x = d.x;
        p.source_y = d.y;
        p.source_width = d.width;
        p.source_height = d.height;
        p.columns = d.columns;
        p.rows = d.rows;
        p.z = d.z;

        const ImageStorage::Placement::CellOffset cell_offset = p.cellOffset(terminal);
        if (terminal->width_px / terminal->cols > 0) p.x_offset = cell_offset.x;
        if (terminal->height_px / terminal->rows > 0) p.y_offset = cell_offset.y;
    }
    if (!storage->addPlacement(alloc, terminal->screens.active, img.id, result.placement_id, p)) {
        p.deinit(terminal->screens.active);
        encodeError(&result, EncodeableError::OutOfMemory);
        return result;
    }

    /* Apply cursor movement setting. This only applies to pin placements:
     * relative placements never move the cursor regardless of C=, just
     * like kitty. */
    switch (p.location.tag) {
    case ImageStorage::Placement::Location::Tag::virtual_:
    case ImageStorage::Placement::Location::Tag::relative: break;
    case ImageStorage::Placement::Location::Tag::pin: {
        PageList::Pin *pin = p.location.pin;
        switch (d.cursor_movement) {
        case command::Display::CursorMovement::none: break;
        case command::Display::CursorMovement::after: {
            /* We use terminal.index to properly handle scroll regions. */
            Screen *screen = terminal->screens.active;
            const ImageStorage::Placement::GridSize size = p.gridSize(img, terminal);
            const uint64_t target_x = ::wisp::terminal::kitty::graphics_storage::satAddU64(
                (uint64_t)pin->x, (uint64_t)size.cols);
            const bool wraps = target_x >= (uint64_t)terminal->cols;
            const uint64_t requested_rows = ::wisp::terminal::kitty::graphics_storage::satAddU64(
                size.rows > 0 ? (uint64_t)size.rows - 1 : (uint64_t)0, wraps ? 1u : 0u);

            /* The requested row count comes from the application and can
             * be as large as u32. Calling terminal.index once for every
             * row could therefore leave the terminal unresponsive for a
             * very long time.
             *
             * First allow enough calls to reach the bottom of the scroll
             * region. Each call after that scrolls the region by one row,
             * so limit those extra calls to one screen. This follows
             * Kitty's behavior while keeping the amount of work bounded. */
            const Terminal::ScrollingRegion region = terminal->scrolling_region;
            const uint64_t rows_before_scroll =
                (screen->cursor.y >= region.top && screen->cursor.y <= region.bottom &&
                 screen->cursor.x >= region.left && screen->cursor.x <= region.right)
                    ? (uint64_t)(region.bottom - screen->cursor.y)
                    : (uint64_t)0;
            const uint64_t capped = ::wisp::terminal::kitty::graphics_storage::satAddU64(
                rows_before_scroll, (uint64_t)terminal->rows);
            /* Wisp: Zig evaluates the `for (0..rows_to_move)` range once;
             * the bound is snapshotted here because `terminal->index()`
             * mutates the screen. */
            const uint64_t rows_to_move = requested_rows < capped ? requested_rows : capped;
            for (uint64_t i = 0; i < rows_to_move; i += 1) {
                if (!terminal->index()) {
                    /* log.warn("failed to move cursor: {}", .{err}) */
                    break;
                }
            }

            /* Kitty wraps once when the movement reaches the right edge;
             * otherwise it leaves the cursor immediately to the right of
             * the placement. */
            screen->cursor.pending_wrap = false;
            screen->cursorHorizontalAbsolute(
                wraps ? (::wisp::vt::size::CellCountInt)0
                      : (::wisp::vt::size::CellCountInt)target_x);
            break;
        }
        }
        break;
    }
    }

    return result;
}

/// Transmit animation frame data (a=f).
///
/// Frame data is loaded exactly like image data, including chunking
/// and every transmission medium, and once complete it is composed
/// into a frame of an existing image's animation.
static Response transmitAnimationFrame(Allocator alloc, Terminal *terminal, const Command *cmd) {
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    /* A chunk arriving while a load is in progress continues that load. */
    if (storage->loading != nullptr) {
        LoadingImage *loading = storage->loading;

        /* If this is the first frame, then we treat it like a normal image
         * because the first frame is just the image. */
        if (!loading->has_frame) return transmit(alloc, terminal, cmd);

        /* Not the first frame, so continue loading similar to transmit
         * but this is for a subsequent frame. */
        Response result = loading->response;
        {
            const EncodeableError err = loading->addData(alloc, cmd->data, cmd->data_len);
            if (err != EncodeableError::none) {
                encodeError(&result, err);
                return result;
            }
        }

        /* If more chunks are expected we don't respond yet. */
        command::Transmission t;
        {
            const bool have = cmd->transmission(&t);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
        }
        if (t.more_chunks) return Response();

        /* Final chunk: copy the loading state out and free the
         * pointer, mirroring loadAndAddImage.
         *
         * Wisp: a move-out, see the header note. */
        LoadingImage loading_copy = *loading;
        alloc.destroy(loading);
        storage->loading = nullptr;
        /* defer loading_copy.deinit(alloc); */
        const Response r = completeAnimationFrame(alloc, terminal, &loading_copy);
        loading_copy.deinit(alloc);
        return r;
    }

    /* We are starting our load, either the first frame or a subsequent frame. */
    const command::AnimationFrameLoading f = cmd->control.transmit_animation_frame;
    const command::Transmission t = f.transmission;
    Response result;
    result.id = t.image_id;
    result.image_number = t.image_number;
    result.placement_id = t.placement_id;
    /* Errors echo the client's frame number; on success this is
     * replaced with the resolved (possibly newly assigned) one. */
    result.frame = f.edit_frame;

    /* A frame can only be added to an existing image. */
    if (t.image_id == 0 && t.image_number == 0) {
        result.message = "EINVAL: image ID or number required";
        result.message_len = strlen(result.message);
        return result;
    }
    Image *img = storage->imagePtrByIdOrNumber(t.image_id, t.image_number);
    if (img == nullptr) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }
    result.id = img->id;

    LoadingImage loading;
    {
        const EncodeableError err =
            LoadingImage::init(alloc, cmd, storage->image_limits, &loading);
        if (err != EncodeableError::none) {
            encodeError(&result, err);
            return result;
        }
    }
    loading.has_frame = true;
    loading.frame.cmd = f;
    loading.frame.image_generation = img->generation;
    loading.response.id = img->id;

    /* Chunked: store the loading state and wait for the rest. The
     * frame parameters above are saved with it; continuation chunks
     * contribute only payload bytes. */
    if (t.more_chunks) {
        LoadingImage *loading_ptr = alloc.create<LoadingImage>();
        if (loading_ptr == nullptr) {
            loading.deinit(alloc);
            encodeError(&result, EncodeableError::OutOfMemory);
            return result;
        }
        /* Wisp: placement-new copy into the raw cell, see the header note. */
        new (loading_ptr) LoadingImage(loading);
        storage->loading = loading_ptr;
        return Response();
    }

    /* defer loading.deinit(alloc); */
    const Response r = completeAnimationFrame(alloc, terminal, &loading);
    loading.deinit(alloc);
    return r;
}

/// Complete a fully loaded animation frame: validate it against its
/// image and compose it into the image's animation.
static Response completeAnimationFrame(Allocator alloc, Terminal *terminal,
                                       LoadingImage *loading) {
    ImageStorage *storage = &terminal->screens.active->kitty_images;
    Response result = loading->response;
    assert(loading->has_frame); /* Wisp: upstream's `loading.frame.?` */
    const command::AnimationFrameLoading f = loading->frame.cmd;

    /* Re-resolve the image: it may have been deleted, evicted, or
     * replaced while the frame data was being transmitted. The saved
     * generation pins the exact image the load started against. */
    Image *img = storage->imagePtrByIdOrNumber(loading->image.id, loading->image.number);
    if (img == nullptr) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }
    if (img->generation != loading->frame.image_generation) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }
    {
        const uint8_t *base;
        size_t base_len;
        if (!img->data.bytes(&base, &base_len)) {
            result.message = "EINVAL: image data incomplete";
            result.message_len = strlen(result.message);
            return result;
        }
    }

    /* Finish decoding the frame data: decompression, PNG decoding,
     * and length validation all match image loading. */
    Image frame_img;
    {
        const EncodeableError err = loading->complete(alloc, &frame_img);
        if (err != EncodeableError::none) {
            encodeError(&result, err);
            return result;
        }
    }
    /* Wisp: upstream's `defer frame_img.deinit(alloc)`. Every exit below
     * goes through this guard. */
    struct FrameGuard {
        Allocator alloc;
        Image *img;
        FrameGuard(Allocator a, Image *i) : alloc(a), img(i) {}
        ~FrameGuard() { img->deinit(alloc); }
    } frame_guard(alloc, &frame_img);

    /* The frame rectangle may not exceed the image's size. The x/y
     * offsets are deliberately not validated: composition clips,
     * matching Kitty. */
    if (frame_img.width > img->width || frame_img.height > img->height) {
        result.message = "EINVAL: frame dimensions exceed image";
        result.message_len = strlen(result.message);
        return result;
    }

    /* All composition happens in RGBA; convert both sides as needed. */
    if (frame_img.format != command::Transmission::Format::rgba) {
        const uint8_t *src_bytes;
        size_t src_bytes_len;
        {
            const bool have = frame_img.data.bytes(&src_bytes, &src_bytes_len);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
        }
        uint8_t *rgba;
        size_t rgba_len;
        if (pixel::rgbaFromFormat(alloc, frame_img.format, src_bytes, src_bytes_len, &rgba,
                                  &rgba_len) != pixel::Error::none) {
            encodeError(&result, EncodeableError::OutOfMemory);
            return result;
        }
        frame_img.data.deinit(alloc);
        frame_img.data = Image::Data::initComplete(rgba, rgba_len);
        frame_img.format = command::Transmission::Format::rgba;
    }
    if (!storage->convertImageToRgba(alloc, img)) {
        encodeError(&result, EncodeableError::OutOfMemory);
        return result;
    }

    animation::Animation *anim = ensureAnimation(alloc, img);
    if (anim == nullptr) {
        encodeError(&result, EncodeableError::OutOfMemory);
        return result;
    }

    /* Resolve the frame number: r in 1..count edits that existing
     * frame, anything else (including omitted) creates a new frame
     * appended at count+1. The resolved number is echoed in the
     * response so clients can learn assigned frame numbers. */
    const uint32_t count = anim->frameCount();
    /* Wisp: upstream's `number:` labeled block. */
    uint32_t number = 0;
    {
        const uint32_t r = f.edit_frame;
        if (r == 0 || r > count + 1) {
            number = count + 1;
        } else {
            number = r;
        }
    }
    result.frame = number;

    const uint8_t *src;
    size_t src_len;
    {
        const bool have = frame_img.data.bytes(&src, &src_len);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }

    if (number == count + 1) {
        /* Creating a new frame. The gap defaults to 40ms when omitted
         * and a negative gap creates a gapless (never shown) frame. */
        uint32_t gap = 0;
        if (f.gap_ms > 0) {
            gap = (uint32_t)f.gap_ms;
        } else if (f.gap_ms < 0) {
            gap = 0;
        } else {
            gap = animation::default_gap_ms;
        }

        /* The base canvas frame must exist before we reserve space. */
        if (f.create_frame > 0) {
            const uint8_t *base;
            size_t base_len;
            if (!img->frameData(f.create_frame, &base, &base_len)) {
                result.message = "EINVAL: base frame not found";
                result.message_len = strlen(result.message);
                return result;
            }
        }

        /* Reserve room for the new frame, evicting other images if
         * needed. Eviction can remove images (never this one), so we
         * re-resolve our pointer afterwards to be safe. */
        const size_t frame_len = (size_t)img->width * (size_t)img->height * 4;
        const uint32_t image_id = img->id;
        if (!storage->reserveAnimationBytes(alloc, terminal->screens.active, image_id,
                                            frame_len)) {
            result.message = "ENOSPC: animation frame storage full";
            result.message_len = strlen(result.message);
            return result;
        }
        img = storage->imagePtrByIdOrNumber(image_id, 0);
        assert(img != nullptr); /* Wisp: upstream's `.?` */

        uint8_t *canvas = alloc.alloc(frame_len);
        if (canvas == nullptr) {
            storage->releaseAnimationBytes(frame_len);
            result.message = "ENOMEM: out of memory";
            result.message_len = strlen(result.message);
            return result;
        }
        if (f.create_frame > 0) {
            const uint8_t *base;
            size_t base_len;
            const bool have = img->frameData(f.create_frame, &base, &base_len);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
            /* Wisp: Zig's `@memcpy` requires equal lengths; frames are
             * always stored at full image size, so this holds. */
            assert(base_len == frame_len);
            memcpy(canvas, base, frame_len);
        } else {
            pixel::fillBackground(canvas, frame_len, f.background);
        }
        pixel::composeRect(canvas, frame_len, img->width, img->height, src, src_len,
                           frame_img.width, frame_img.height, f.x, f.y, f.composition_mode);

        {
            animation::Animation::Frame new_frame;
            new_frame.data = canvas;
            new_frame.data_len = frame_len;
            new_frame.gap_ms = gap;
            if (!anim->frames.append(alloc, new_frame)) {
                alloc.free(canvas, frame_len);
                storage->releaseAnimationBytes(frame_len);
                result.message = "ENOMEM: out of memory";
                result.message_len = strlen(result.message);
                return result;
            }
        }

        /* A new frame never changes the displayed frame (playback
         * reaches it later), but the storage content changed. */
        storage->markMutated();
    } else {
        /* Editing an existing frame. A nonzero gap also updates the
         * frame's gap; the 40ms default doesn't apply to edits.
         *
         * Unlike frame creation there is no byte reservation here:
         * frames are always stored at full image size, so the edit
         * composes into the existing buffer in place and storage
         * usage cannot change. Kitty likewise exempts frame edits
         * from its quota check. */
        if (f.gap_ms != 0) {
            anim->setGapAt(number - 1, f.gap_ms > 0 ? (uint32_t)f.gap_ms : 0);
        }

        /* The frame data is owned by this storage, so the const cast
         * is safe (same reasoning as the renderer's image uploads). */
        const uint8_t *dst_const;
        size_t dst_len;
        {
            const bool have = img->frameData(number, &dst_const, &dst_len);
            assert(have); /* Wisp: upstream's `.?` */
            (void)have;
        }
        uint8_t *dst = (uint8_t *)dst_const;
        pixel::composeRect(dst, dst_len, img->width, img->height, src, src_len, frame_img.width,
                           frame_img.height, f.x, f.y, f.composition_mode);

        if (number - 1 == anim->current_index) {
            /* The displayed pixels changed; restart the frame's gap
             * timer like Kitty's re-upload does. */
            anim->has_frame_shown_at_ms = false;
            storage->markImageContentChanged(img);
        } else {
            storage->markMutated();
        }
    }

    return result;
}

/// Control animation playback (a=a).
///
/// Successful commands never respond. The only possible responses are
/// for a missing image or identifier. Invalid key values are silently
/// ignored, matching Kitty.
static Response controlAnimation(Allocator alloc, Terminal *terminal, const Command *cmd) {
    const command::AnimationControl a = cmd->control.control_animation;
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    Response result;
    result.id = a.image_id;
    result.image_number = a.image_number;
    result.placement_id = a.placement_id;
    if (a.image_id == 0 && a.image_number == 0) {
        result.message = "EINVAL: image ID or number required";
        result.message_len = strlen(result.message);
        return result;
    }
    Image *img = storage->imagePtrByIdOrNumber(a.image_id, a.image_number);
    if (img == nullptr) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }

    animation::Animation *anim = ensureAnimation(alloc, img);
    if (anim == nullptr) {
        encodeError(&result, EncodeableError::OutOfMemory);
        return result;
    }

    /* The keys below are applied independently and in the same order as Kitty. */

    /* Set a frame's gap (r= together with z=). This is the only way
     * to give the root frame a gap, since it is created gapless. */
    if (a.frame != 0 && a.frame <= anim->frameCount() && a.gap_ms != 0) {
        anim->setGapAt(a.frame - 1, a.gap_ms > 0 ? (uint32_t)a.gap_ms : 0);
        storage->markMutated();
    }

    /* Set the current frame (c=), the client-driven animation
     * primitive. */
    if (a.current_frame != 0 && a.current_frame <= anim->frameCount() &&
        a.current_frame - 1 != anim->current_index) {
        anim->current_index = a.current_frame - 1;
        anim->has_frame_shown_at_ms = false;
        storage->markImageContentChanged(img);
    }

    /* Set the playback state (s=). Any state change resets the loop
     * counter; leaving the stopped state restarts the gap timer. */
    if (a.action != command::AnimationControl::AnimationAction::invalid) {
        const animation::Animation::State old = anim->state;
        /* Wisp: value-initialized from the current state. Upstream's
         * `.invalid => unreachable` arm is guarded by the `if` above;
         * NDEBUG drops the assert, so the state is left unchanged
         * instead of being read uninitialized. */
        animation::Animation::State new_state = anim->state;
        switch (a.action) {
        case command::AnimationControl::AnimationAction::invalid: assert(false); break;
        case command::AnimationControl::AnimationAction::stop:
            new_state = animation::Animation::State::stopped;
            break;
        case command::AnimationControl::AnimationAction::run_wait:
            new_state = animation::Animation::State::loading;
            break;
        case command::AnimationControl::AnimationAction::run:
            new_state = animation::Animation::State::running;
            break;
        }
        anim->state = new_state;
        if (old == animation::Animation::State::stopped &&
            anim->state != animation::Animation::State::stopped) {
            anim->has_frame_shown_at_ms = false;
        }
        anim->current_loop = 0;
        storage->markMutated();
    }

    /* Set the loop count (v=), stored off by one per the protocol:
     * v=1 loops forever, v=n plays n-1 loops. */
    if (a.loops != 0) {
        anim->max_loops = a.loops - 1;
        storage->markMutated();
    }

    /* Successful animation control commands never respond. */
    return Response();
}

/// Compose a rectangle of pixels from one animation frame onto
/// another (a=c). Frame 1 (the root frame) always exists, so this also works
/// on images without any animation state.
static Response composeAnimation(Allocator alloc, Terminal *terminal, const Command *cmd) {
    const command::AnimationFrameComposition c = cmd->control.compose_animation;
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    Response result;
    result.id = c.image_id;
    result.image_number = c.image_number;
    result.placement_id = c.placement_id;
    if (c.image_id == 0 && c.image_number == 0) {
        result.message = "EINVAL: image ID or number required";
        result.message_len = strlen(result.message);
        return result;
    }
    Image *img = storage->imagePtrByIdOrNumber(c.image_id, c.image_number);
    if (img == nullptr) {
        result.message = "ENOENT: image not found";
        result.message_len = strlen(result.message);
        return result;
    }
    result.id = img->id;
    {
        const uint8_t *base;
        size_t base_len;
        if (!img->data.bytes(&base, &base_len)) {
            result.message = "EINVAL: image data incomplete";
            result.message_len = strlen(result.message);
            return result;
        }
    }

    /* Both frames must exist. Note that r is the source frame and c
     * the destination; the spec's reference table describes these
     * backwards (see AnimationFrameComposition). */
    {
        const uint8_t *p;
        size_t p_len;
        if (!img->frameData(c.source_frame, &p, &p_len)) {
            result.message = "ENOENT: source frame not found";
            result.message_len = strlen(result.message);
            return result;
        }
    }
    {
        const uint8_t *p;
        size_t p_len;
        if (!img->frameData(c.dest_frame, &p, &p_len)) {
            result.message = "ENOENT: destination frame not found";
            result.message_len = strlen(result.message);
            return result;
        }
    }

    /* Rectangle validation is done in u64 so untrusted 32-bit values
     * can't overflow. Out-of-bounds rectangles are errors here,
     * unlike a=f which clips. */
    const uint64_t width = c.width > 0 ? (uint64_t)c.width : (uint64_t)img->width;
    const uint64_t height = c.height > 0 ? (uint64_t)c.height : (uint64_t)img->height;
    if ((uint64_t)c.x + width > (uint64_t)img->width ||
        (uint64_t)c.y + height > (uint64_t)img->height) {
        result.message = "EINVAL: destination rectangle out of bounds";
        result.message_len = strlen(result.message);
        return result;
    }
    if ((uint64_t)c.left_edge + width > (uint64_t)img->width ||
        (uint64_t)c.top_edge + height > (uint64_t)img->height) {
        result.message = "EINVAL: source rectangle out of bounds";
        result.message_len = strlen(result.message);
        return result;
    }

    /* Composing a frame onto itself requires non-overlapping
     * rectangles. */
    if (c.source_frame == c.dest_frame) {
        const uint64_t x_max = c.left_edge > c.x ? (uint64_t)c.left_edge : (uint64_t)c.x;
        const uint64_t x_min = c.left_edge < c.x ? (uint64_t)c.left_edge : (uint64_t)c.x;
        const bool x_overlaps = x_max < x_min + width;
        const uint64_t y_max = c.top_edge > c.y ? (uint64_t)c.top_edge : (uint64_t)c.y;
        const uint64_t y_min = c.top_edge < c.y ? (uint64_t)c.top_edge : (uint64_t)c.y;
        const bool y_overlaps = y_max < y_min + height;
        if (x_overlaps && y_overlaps) {
            result.message = "EINVAL: source and destination rectangles overlap";
            result.message_len = strlen(result.message);
            return result;
        }
    }

    /* All composition happens in RGBA. */
    if (!storage->convertImageToRgba(alloc, img)) {
        encodeError(&result, EncodeableError::OutOfMemory);
        return result;
    }

    /* The frame data is owned by this storage, so the const cast is
     * safe. The rectangles were validated disjoint above, so in-place
     * composition within one frame is well-defined. */
    const uint8_t *src;
    size_t src_len;
    {
        const bool have = img->frameData(c.source_frame, &src, &src_len);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }
    const uint8_t *dst_const;
    size_t dst_len;
    {
        const bool have = img->frameData(c.dest_frame, &dst_const, &dst_len);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }
    uint8_t *dst = (uint8_t *)dst_const;
    pixel::composeCanvasRect(dst, dst_len, src, src_len, img->width, (uint32_t)width,
                             (uint32_t)height, c.left_edge, c.top_edge, c.x, c.y,
                             c.composition_mode);

    /* If the destination is the displayed frame then the on-screen
     * content changed. */
    const uint32_t current = img->animation_ != nullptr ? img->animation_->current_index : 0;
    if (c.dest_frame - 1 == current) {
        storage->markImageContentChanged(img);
    } else {
        storage->markMutated();
    }

    return result;
}

/// Get or lazily create the animation state for an image.
///
/// Wisp: `Allocator.Error!*animation.Animation` is a nullable pointer.
static animation::Animation *ensureAnimation(Allocator alloc, Image *img) {
    if (img->animation_ != nullptr) return img->animation_;
    animation::Animation *anim = alloc.create<animation::Animation>();
    if (anim == nullptr) return nullptr;
    new (anim) animation::Animation();
    img->animation_ = anim;
    return anim;
}

/// Display a previously transmitted image.
static Response delete_(Allocator alloc, Terminal *terminal, const Command *cmd) {
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    /* Every delete command aborts an incomplete chunked upload. */
    if (storage->loading != nullptr) {
        LoadingImage *loading = storage->loading;
        loading->destroy(alloc);
        storage->loading = nullptr;
    }

    /* Then perform the actual deletion request, too. */
    storage->delete_(alloc, terminal, cmd->control.delete_.action);

    /* Delete never responds on success */
    return Response();
}

static EncodeableError loadAndAddImage(Allocator alloc, Terminal *terminal, const Command *cmd,
                                       Load *out) {
    command::Transmission t;
    {
        const bool have = cmd->transmission(&t);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
    }
    ImageStorage *storage = &terminal->screens.active->kitty_images;

    /* Determine our image. This also handles chunking and early exit.
     * Wisp: upstream's `loading:` labeled block. */
    LoadingImage loading;
    if (storage->loading != nullptr) {
        LoadingImage *loading_ptr = storage->loading;

        /* Note: we do NOT want to call "cmd.toOwnedData" here because
         * we're _copying_ the data. We want the command data to be freed. */
        {
            const EncodeableError err = loading_ptr->addData(alloc, cmd->data, cmd->data_len);
            if (err != EncodeableError::none) return err;
        }

        /* If we have more then we're done */
        if (t.more_chunks) {
            out->image = loading_ptr->image;
            out->more = true;
            return EncodeableError::none;
        }

        /* We have no more chunks. We're going to be completing the
         * image so we want to destroy the pointer to the loading
         * image and copy it out.
         *
         * Wisp: a move-out, see the header note. */
        loading = *loading_ptr;
        alloc.destroy(loading_ptr);
        storage->loading = nullptr;
    } else {
        /* Reusing a specific image ID deletes the old image and all of its
         * placements when the new transmission begins, not when it completes. */
        if (t.image_id > 0) {
            command::Delete::Action action;
            action.tag = command::Delete::Action::Tag::id;
            action.id.image_id = t.image_id;
            action.id.delete_ = true;
            storage->delete_(alloc, terminal, action);
        }

        const EncodeableError err =
            LoadingImage::init(alloc, cmd, storage->image_limits, &loading);
        if (err != EncodeableError::none) return err;
    }

    /* We only want to deinit on error. If we're chunking, then we don't
     * want to deinit at all. If we're not chunking, then we'll deinit
     * after we've copied the image out.
     *
     * Wisp: upstream's `errdefer loading.deinit(alloc)` is spelled out at
     * each of the three error exits below. */

    /* If the image has no ID, we assign one */
    if (loading.image.id == 0) {
        if (loading.image.number > 0) {
            loading.image.id = storage->nextImageId(ImageStorage::NextImageIdMode::explicit_);
        } else {
            loading.image.id = storage->nextImageId(ImageStorage::NextImageIdMode::implicit);
            loading.image.metadata.implicit_id = true;
        }
    }

    /* If this is chunked, this is the beginning of a new chunked transmission.
     * (We checked for an in-progress chunk above.) */
    if (t.more_chunks) {
        /* We allocate the pointer on the heap because its rare and we
         * don't want to always pay the memory cost to keep it around. */
        LoadingImage *loading_ptr = alloc.create<LoadingImage>();
        if (loading_ptr == nullptr) {
            loading.deinit(alloc);
            return EncodeableError::OutOfMemory;
        }
        /* Wisp: placement-new copy into the raw cell, see the header note. */
        new (loading_ptr) LoadingImage(loading);
        storage->loading = loading_ptr;
        out->image = loading.image;
        out->more = true;
        return EncodeableError::none;
    }

    /* Dump the image data before it is decompressed
     * loading.debugDump() catch unreachable; */

    /* Validate and store our image */
    Image img;
    {
        const EncodeableError err = loading.complete(alloc, &img);
        if (err != EncodeableError::none) {
            loading.deinit(alloc);
            return err;
        }
    }
    if (!storage->addImage(alloc, terminal->screens.active, img)) {
        img.deinit(alloc);
        loading.deinit(alloc);
        return EncodeableError::OutOfMemory;
    }

    /* Get our display settings */
    const bool has_display_ = loading.has_display;
    const command::Display display_ = loading.display;

    /* Ensure we deinit the loading state because we're done. The image
     * won't be deinit because of "complete" above. */
    loading.deinit(alloc);

    out->image = img;
    out->has_display = has_display_;
    out->display = display_;
    return EncodeableError::none;
}

/// Encode an error code into a message for a response.
static void encodeError(Response *r, EncodeableError err) {
    switch (err) {
    case EncodeableError::OutOfMemory: r->message = "ENOMEM: out of memory"; break;
    case EncodeableError::InsufficientData: r->message = "ENODATA: insufficient data"; break;
    case EncodeableError::InvalidData: r->message = "EINVAL: invalid data"; break;
    case EncodeableError::DecompressionFailed: r->message = "EINVAL: decompression failed"; break;
    case EncodeableError::FilePathTooLong: r->message = "EINVAL: file path too long"; break;
    case EncodeableError::TemporaryFileNotInTempDir:
        r->message = "EINVAL: temporary file not in temp dir";
        break;
    case EncodeableError::TemporaryFileNotNamedCorrectly:
        r->message = "EINVAL: temporary file not named correctly";
        break;
    case EncodeableError::UnsupportedFormat: r->message = "EINVAL: unsupported format"; break;
    case EncodeableError::UnsupportedMedium: r->message = "EINVAL: unsupported medium"; break;
    case EncodeableError::UnsupportedDepth: r->message = "EINVAL: unsupported pixel depth"; break;
    case EncodeableError::DimensionsRequired: r->message = "EINVAL: dimensions required"; break;
    case EncodeableError::DimensionsTooLarge: r->message = "EINVAL: dimensions too large"; break;

    /* Wisp: not in upstream's `Image.Error || Allocator.Error`; see the
     * header note. These cannot reach here. */
    case EncodeableError::none:
    case EncodeableError::UncPath:
    case EncodeableError::DevicePath:
    case EncodeableError::ReservedDeviceName:
    case EncodeableError::NotDriveAbsolute: r->message = "EINVAL: invalid data"; break;
    }
    r->message_len = strlen(r->message);
}

} /* namespace graphics_exec */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_EXEC_HPP */
