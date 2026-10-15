/* Transliterated from Ghostty src/terminal/kitty/graphics_storage.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: out-of-line definitions for graphics_storage.hpp. `Screen` carries
 * `ImageStorage` by value, so graphics_storage.hpp is included *by*
 * screen.hpp and can only forward-declare `Screen` and `Terminal`; every
 * body that dereferences one therefore lives here or in
 * graphics_storage_screen_impl.hpp. Nothing was rewritten in the move: the
 * bodies, their comments and their order are graphics_storage.zig's.
 *
 * This file holds the bodies that need only `Screen`. screen.hpp includes it
 * at its end, where `Screen` is complete, so a translation unit that uses
 * `Screen` alone (e.g. tests/test_vt_screen_port.cpp) links.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_SCREEN_IMPL_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_SCREEN_IMPL_HPP

#include "../../vt/screen.hpp"
#include "graphics_storage.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_storage {

inline void ImageStorage::Placement::deinit(Screen *s) const {
    switch (location.tag) {
    case Location::Tag::pin: s->pages.untrackPin(location.pin); break;
    case Location::Tag::virtual_:
    case Location::Tag::relative: break;
    }
}

inline void ImageStorage::ScrollMargins::end() {
    /* Wisp: `for (0..n)` snapshot; nothing here mutates the list,
     * but the bound is snapshotted on principle. */
    const size_t n = restores.items_len;
    for (size_t i = 0; i < n; i += 1) {
        const Restore &restore = restores.items[i];
        const Maybe<PageList::Pin> p = screen->pages.pin(
            point::Point::active(restore.x, restore.y));
        if (!p.has) continue;
        *restore.pin = p.value;
    }
    restores.deinit(screen->alloc);
}

inline void ImageStorage::deinit(Allocator alloc, Screen *s) {
    if (loading != nullptr) loading->destroy(alloc);

    clearPlacements(s);
    placements.deinit(alloc);

    ImageMap::Iterator it = images.iterator();
    ImageMap::Entry kv;
    while (it.next(&kv)) kv.value_ptr->deinit(alloc);
    images.deinit(alloc);
}

inline void ImageStorage::setLimit(Allocator alloc, Screen *s, size_t limit) {
    /* Special case disabling by quickly deleting all */
    if (limit == 0) {
        const LoadingImage::Limits saved_limits = image_limits;
        deinit(alloc, s);
        *this = ImageStorage();
        image_limits = saved_limits;
        markMutated();
    }

    /* If we re lowering our limit, check if we need to evict.
     * Wisp: upstream does not return from the limit == 0 branch
     * above; the fall-through is kept. */
    if (limit < total_bytes) {
        const size_t req_bytes = total_bytes - limit;
        /* log.info("evicting images to lower limit, evicting={}", req_bytes) */
        if (!evictImage(alloc, s, req_bytes)) {
            /* log.warn("failed to evict enough images for required bytes") */
        }
    }

    total_limit = limit;
}

inline bool ImageStorage::addImage(Allocator alloc, Screen *s, const Image &img) {
    const size_t new_len = img.data.len();

    /* If the image itself is over the limit, then error immediately */
    if (new_len > total_limit) return false;

    /* Credit an existing image's reservation before calculating the
     * replacement size. In particular, a completed live transmission
     * replacing pending snapshot metadata must be able to reuse the
     * reservation without evicting its own ID. */
    size_t old_len = 0;
    {
        Image old;
        if (images.get(img.id, &old)) old_len = old.storageSize();
    }
    assert(old_len <= total_bytes);

    /* Reserve map capacity before evicting anything so allocation failure
     * cannot leave storage partially evicted. Existing keys need no room. */
    if (!images.contains(img.id)) {
        if (!images.ensureUnusedCapacity(alloc, 1)) return false;
    }

    /* If this would put us over the limit, then evict. */
    const size_t total = total_bytes - old_len + new_len;
    if (total > total_limit) {
        const size_t req_bytes = total - total_limit;
        /* log.info("evicting images to make space for {} bytes", req_bytes) */
        if (!evictImageExcept(alloc, s, req_bytes, Maybe<uint32_t>(img.id))) {
            /* log.warn("failed to evict enough images for required bytes") */
            return false;
        }
    }

    const ImageMap::GetOrPutResult gop = images.getOrPutAssumeCapacity(img.id);

    /* log.debug("addImage image={}", img.withoutData()) */

    if (gop.found_existing) {
        /* Retransmitting a specific image ID replaces the old image and all
         * of its placements, as required by the Kitty graphics protocol. */
        removePlacementsByImageId(s, img.id);

        /* Relative placements parented to the removed placements go too. */
        (void)removeOrphans(s, nullptr);

        /* Replacing an image drops its animation frames with it,
         * implementing the protocol rule that retransmitting the
         * base image resets the animation. */
        total_bytes -= gop.value_ptr->storageSize();
        gop.value_ptr->deinit(alloc);
    }

    *gop.value_ptr = img;
    gop.value_ptr->metadata.placement_count = 0;
    total_bytes += new_len;

    /* Stamp the stored image with a fresh generation. This gives
     * every add/replace a unique stamp even when the same image ID
     * is retransmitted with identical dimensions, so consumers
     * (e.g. renderer texture caches) can detect content changes. */
    markMutated();
    gop.value_ptr->generation = generation;
    return true;
}

inline bool ImageStorage::addPendingImage(Allocator alloc, Screen *s, const Image &img,
                                          PendingImage *out) {
    assert(img.data.tag == Image::Data::Tag::pending);
    if (!addImage(alloc, s, img)) return false;
    Image stored;
    const bool have = images.get(img.id, &stored);
    assert(have); /* Wisp: upstream's `.?` */
    (void)have;
    out->id = stored.id;
    out->generation = stored.generation;
    return true;
}

inline bool ImageStorage::addPlacement(Allocator alloc, Screen *s, uint32_t image_id,
                                       uint32_t placement_id, const Placement &p) {
    assert(images.getPtr(image_id) != nullptr);
    /* log.debug("placement image_id={} placement_id={} placement={}") */

    /* When we add a placement, take this opportunity to reap garbage.
     * Garbage are placements that disappeared off scrollback (were
     * pruned). We don't clear Kitty state in the hot path so we do
     * this opportunistically here. */
    reapGarbagePlacements(s);

    /* The important piece here is that the placement ID needs to
     * be marked internal if it is zero. This allows multiple placements
     * to be added for the same image. If it is non-zero, then it is
     * an external placement ID and we can only have one placement
     * per (image id, placement id) pair. */
    PlacementKey key;
    key.image_id = image_id;
    if (placement_id == 0) {
        /* Wisp: upstream's `defer self.next_internal_placement_id +%= 1`
         * inside the labeled block yields the pre-increment value. */
        const uint32_t internal_id = next_internal_placement_id;
        next_internal_placement_id += 1; /* `+%=` */
        key.placement_id = PlacementId::internal(internal_id);
    } else {
        key.placement_id = PlacementId::external(placement_id);
    }

    PlacementMap::GetOrPutResult gop;
    if (!placements.getOrPut(alloc, key, &gop)) return false;
    if (gop.found_existing) {
        gop.value_ptr->deinit(s);
    } else {
        Image *img = images.getPtr(image_id);
        assert(img != nullptr); /* Wisp: upstream's `.?` */
        /* Wisp: `std.math.add(u30, placement_count, 1) catch` — see the
         * placement_count_max note at the top of this file. */
        if (img->metadata.placement_count >= placement_count_max) {
            placements.removeByPtr(gop.key_ptr);
            return false;
        }
        img->metadata.placement_count += 1;
    }
    *gop.value_ptr = p;

    /* This always mutates */
    markMutated();
    return true;
}

inline void ImageStorage::reapGarbagePlacements(Screen *s) {
    bool removed = false;
    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) {
        if (entry.value_ptr->location.tag != Placement::Location::Tag::pin) continue;
        PageList::Pin *pin = entry.value_ptr->location.pin;
        if (!pin->garbage) continue;

        removePlacement(s, entry);
        removed = true;
    }
    if (!removed) return;

    /* If we removed a placement, then also remove any orphan
     * children if this was a parent. */
    (void)removeOrphans(s, nullptr);

    markMutated();
}

inline void ImageStorage::clearPlacements(Screen *s) {
    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) entry.value_ptr->deinit(s);
    placements.clearRetainingCapacity();
}

inline void ImageStorage::removePlacementsByImageId(Screen *s, uint32_t image_id) {
    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) {
        if (entry.key_ptr->image_id != image_id) continue;
        removePlacement(s, entry);
    }
}

inline void ImageStorage::removePlacement(Screen *s, const PlacementMap::Entry &entry) {
    entry.value_ptr->deinit(s);
    Image *img = images.getPtr(entry.key_ptr->image_id);
    assert(img != nullptr); /* Wisp: upstream's `.?` */
    assert(img->metadata.placement_count > 0);
    img->metadata.placement_count -= 1;
    placements.removeByPtr(entry.key_ptr);
}

inline bool ImageStorage::removeOrphans(Screen *s, const Allocator *delete_unused) {
    bool removed_any = false;
    bool removed = true;
    while (removed) {
        removed = false;
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            if (entry.value_ptr->location.tag != Placement::Location::Tag::relative)
                continue;
            const Placement::Relative rel = entry.value_ptr->location.relative;
            if (placements.contains(rel.parent)) continue;

            /* Parent is gone, remove this placement. */
            const uint32_t image_id = entry.key_ptr->image_id;
            removePlacement(s, entry);
            removed = true;
            removed_any = true;

            if (delete_unused != nullptr) deleteIfUnused(*delete_unused, image_id);
        }
    }

    return removed_any;
}

inline ImageStorage::ParentError ImageStorage::resolveParent(Screen *s,
                                                             const Maybe<PlacementKey> &child,
                                                             uint32_t parent_image_id,
                                                             uint32_t parent_placement_id,
                                                             PlacementKey *out) {
    /* Reap placements whose content has been pruned from history
     * before resolving: a pruned parent must be reported as missing,
     * not accepted and then immediately reaped by addPlacement's own
     * sweep, which would store an orphan. */
    reapGarbagePlacements(s);

    /* If the parent doesn't exist we're already failed. */
    if (!images.contains(parent_image_id)) {
        return ParentError::ParentImageNotFound;
    }

    /* Find the parent. Wisp: upstream's `parent:` labeled block. */
    PlacementKey parent;
    {
        /* An explicit parent placement ID (Q=) selects exactly that
         * placement. */
        if (parent_placement_id > 0) {
            const PlacementKey key(parent_image_id,
                                   PlacementId::external(parent_placement_id));
            if (!placements.contains(key)) {
                return ParentError::ParentPlacementNotFound;
            }
            parent = key;
        } else {
            /* No Q: pick a placement of the parent image. Kitty picks
             * the oldest surviving placement; our placement map doesn't
             * track creation order so we pick by PlacementId.preferredOver
             * instead. This is unspecified so any behavior here is fine.
             * In practice the parent image has a single placement, and
             * clients use Q when it doesn't. */
            Maybe<PlacementId> best = Maybe<PlacementId>::none();
            PlacementMap::KeyIterator it = placements.keyIterator();
            for (PlacementKey *key = it.next(); key != nullptr; key = it.next()) {
                if (key->image_id != parent_image_id) continue;
                const PlacementId id = key->placement_id;
                if (!best.has) {
                    best = Maybe<PlacementId>(id);
                    continue;
                }
                if (id.preferredOver(best.value)) best = Maybe<PlacementId>(id);
            }
            if (!best.has) return ParentError::ParentPlacementNotFound;

            parent = PlacementKey(parent_image_id, best.value);
        }
    }

    /* A placement cannot be its own parent. This must be checked
     * before the chain walk so it reports EINVAL, not ECYCLE. */
    if (child.has && parent.eql(child.value)) return ParentError::SelfParent;

    /* Walk the would-be ancestor chain. `depth` counts parent links,
     * starting at one for the link we are about to create. */
    size_t depth = 1;
    PlacementKey key = parent;
    while (true) {
        if (child.has && key.eql(child.value)) return ParentError::Cycle;
        Placement p;
        if (!placements.get(key, &p)) return ParentError::AncestorNotFound;
        /* A pin or virtual placement is the chain root. */
        if (p.location.tag != Placement::Location::Tag::relative) break;
        const Placement::Relative rel = p.location.relative;
        if (depth >= parent_chain_limit) return ParentError::TooDeep;
        depth += 1;
        key = rel.parent;
    }

    *out = parent;
    return ParentError::none;
}

inline bool ImageStorage::reserveAnimationBytes(Allocator alloc, Screen *s, uint32_t image_id,
                                                size_t bytes) {
    if (bytes > total_limit) return false;

    const size_t total = total_bytes + bytes;
    if (total > total_limit) {
        const size_t req_bytes = total - total_limit;
        /* Excess this large cannot be recovered by evicting other
         * images (evictImageExcept also requires it). */
        if (req_bytes > total_limit) return false;
        /* log.info("evicting images for animation frame, evicting={}", req_bytes) */
        if (!evictImageExcept(alloc, s, req_bytes, Maybe<uint32_t>(image_id))) {
            /* log.warn("failed to evict enough images for animation frame") */
            return false;
        }
    }

    total_bytes += bytes;
    return true;
}

inline void ImageStorage::deleteById(Allocator alloc, Screen *s, uint32_t image_id,
                                     uint32_t placement_id, bool delete_unused) {
    bool matched = placement_id == 0;

    /* If no placement, we delete all placements with the ID */
    if (placement_id == 0) {
        removePlacementsByImageId(s, image_id);
    } else {
        PlacementMap::Entry entry;
        if (placements.getEntry(PlacementKey(image_id, PlacementId::external(placement_id)),
                                &entry)) {
            removePlacement(s, entry);
            matched = true;
        }
    }

    /* A placement ID narrows the selection to that exact placement, so an
     * unmatched selector must not free otherwise-unreferenced image data.
     * https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images */
    if (delete_unused && matched) deleteIfUnused(alloc, image_id);
}

inline void ImageStorage::deleteAnimationFrame(Allocator alloc, Screen *s,
                          const command::Delete::Action::AnimationFrames &v) {
    if (v.image_id == 0 && v.image_number == 0) {
        /* log.warn("delete animation frames requires image id or number") */
        return;
    }
    Image *img = imagePtrByIdOrNumber(v.image_id, v.image_number);
    if (img == nullptr) {
        /* log.warn("delete animation frames for unknown image id={} number={}") */
        return;
    }

    /* Wisp: upstream's `anim:` labeled block. */
    animation::Animation *anim = nullptr;
    if (img->animation_ != nullptr && img->animation_->frames.items_len > 0) {
        anim = img->animation_;
    } else {
        /* The image is not (or no longer) an animation. The
         * uppercase delete removes the entire image, even when it
         * still has placements. */
        if (!v.delete_) return;
        removePlacementsByImageId(s, img->id);
        ImageMap::Entry entry;
        const bool have = images.getEntry(img->id, &entry);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
        total_bytes -= entry.value_ptr->storageSize();
        entry.value_ptr->deinit(alloc);
        images.removeByPtr(entry.key_ptr);
        return;
    }

    /* Clamp the frame number: zero selects the root frame and
     * values past the end select the last frame. */
    const uint32_t count = anim->frameCount();
    uint32_t number = v.frame < count ? v.frame : count;
    if (number == 0) number = 1;

    if (number == 1) {
        /* Deleting the root frame promotes frame 2 to root. The
         * promoted frame's bytes stay reserved; only the old root
         * data is freed. */
        releaseAnimationBytes(img->data.len());
        img->data.deinit(alloc);
        const animation::Animation::Frame promoted = orderedRemoveFrame(anim, 0);
        img->data = Image::Data::initComplete(promoted.data, promoted.data_len);
        anim->root_gap_ms = promoted.gap_ms;
    } else {
        const animation::Animation::Frame removed = orderedRemoveFrame(anim, number - 2);
        releaseAnimationBytes(removed.data_len);
        alloc.free(removed.data, removed.data_len);
    }

    /* Fix up the current frame. */
    const uint32_t removed_idx = number == 1 ? 0 : number - 2;
    const uint32_t remaining = (uint32_t)anim->frames.items_len;
    if (anim->current_index > remaining) {
        anim->current_index = remaining;
        anim->has_frame_shown_at_ms = false;
        markImageContentChanged(img);
        return;
    }
    if (removed_idx == anim->current_index) {
        anim->has_frame_shown_at_ms = false;
        markImageContentChanged(img);
    } else {
        if (removed_idx < anim->current_index) anim->current_index -= 1;
        markMutated();
    }
}

inline bool ImageStorage::evictImage(Allocator alloc, Screen *s, size_t req) {
    return evictImageExcept(alloc, s, req, Maybe<uint32_t>::none());
}

inline bool ImageStorage::evictImageExcept(Allocator alloc, Screen *s, size_t req,
                      const Maybe<uint32_t> &exclude_id) {
    assert(req <= total_limit);

    /* Evicting anything is a content mutation. This matters for the
     * setLimit path in particular, which doesn't otherwise mark it.
     * Evicted placements can also orphan relative placements of
     * other images, which must be reaped along with them.
     *
     * Wisp: upstream's `defer` is this RAII guard, so it also runs
     * on the early `return false` from the candidate search. */
    struct EvictGuard {
        ImageStorage *self;
        Screen *s;
        size_t images_before;

        EvictGuard(ImageStorage *st, Screen *sc)
            : self(st), s(sc), images_before(st->images.count()) {}
        ~EvictGuard() {
            if (self->images.count() != images_before) {
                (void)self->removeOrphans(s, nullptr);
                self->markMutated();
            }
        }
    } evict_guard(this, s);

    size_t evicted = 0;
    while (evicted < req) {
        /* Wisp: upstream's `candidate:` labeled block. */
        Candidate c;
        {
            Maybe<Candidate> best = Maybe<Candidate>::none();
            ImageMap::Iterator it = images.iterator();
            ImageMap::Entry kv;
            while (it.next(&kv)) {
                Image *img = kv.value_ptr;
                if (exclude_id.has && img->id == exclude_id.value) continue;

                const Candidate current = Candidate::init(img);
                if (!best.has || current.lessThan(best.value))
                    best = Maybe<Candidate>(current);
            }
            if (!best.has) return false;
            c = best.value;
        }

        {
            PlacementMap::Iterator p_it = placements.iterator();
            PlacementMap::Entry entry;
            while (p_it.next(&entry)) {
                if (entry.key_ptr->image_id == c.id) {
                    removePlacement(s, entry);
                }
            }
        }

        ImageMap::Entry entry;
        const bool have = images.getEntry(c.id, &entry);
        assert(have); /* Wisp: upstream's `.?` */
        (void)have;
        const size_t image_len = entry.value_ptr->storageSize();
        /* log.info("evicting image id={} bytes={}", c.id, image_len) */

        evicted += image_len;
        total_bytes -= image_len;

        entry.value_ptr->deinit(alloc);
        images.removeByPtr(entry.key_ptr);
    }

    return true;
}

} /* namespace graphics_storage */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_SCREEN_IMPL_HPP */
