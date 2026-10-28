/* Transliterated from Ghostty src/input/key_mods.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - Packed structs (Mods, Mods.Keys, Mods.Side, RemapSet.Mask) are plain
 *     structs with `int()` and `fromInt()` giving the same bit layout.
 *   - `builtin.target.os.tag.isDarwin()` is resolved for this Windows port:
 *     `translation` never unsets alt and `ctrlOrSuper` checks ctrl. The
 *     Darwin-only "translation macos-option-as-alt" test is skipped upstream
 *     on every other OS, so it has no counterpart here.
 *   - `std.AutoArrayHashMapUnmanaged(Mods, Mods)` is an insertion-ordered
 *     vector of pairs with the operations RemapSet uses: put (replace in
 *     place or append), swapRemove, get, clone. `finalize`'s sort uses a
 *     comparator that only asks "does `a` have a right side?", i.e. a
 *     partition putting right-sided keys first; upstream's sort is unstable,
 *     this one keeps relative order within each group.
 *   - Storage is std::vector, so allocation failure is not reported.
 *   - `formatEntry` is a template over the formatter, as upstream's
 *     `anytype`; it is called with config/formatter.hpp's EntryFormatter.
 */
#pragma once
#ifndef WISP_INPUT_KEY_MODS_HPP
#define WISP_INPUT_KEY_MODS_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../datastruct/comparison.hpp"
#include "config.hpp"

namespace wisp {
namespace input {

/* Single modifier */
enum class Mod : uint8_t {
    shift,
    ctrl,
    alt,
    super,
};

/* Wisp: Mod.Side */
enum class ModSide : uint8_t { left, right };

/* Aliases for modifier names. */
struct ModAlias {
    const char *name;
    Mod mod;
};
static const ModAlias alias[] = {
    {"cmd", Mod::super},
    {"command", Mod::super},
    {"opt", Mod::alt},
    {"option", Mod::alt},
    {"control", Mod::ctrl},
};

/* A bitmask for all key modifiers.
 *
 * IMPORTANT: Any changes here update include/ghostty.h */
struct Mods {
    typedef uint16_t Backing;

    /* The standard modifier keys only. Does not include the lock keys,
     * only standard bindable keys. */
    struct Keys {
        typedef uint8_t Backing; /* u4 */

        bool shift = false;
        bool ctrl = false;
        bool alt = false;
        bool super = false;

        Backing int_() const {
            return (Backing)((shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0) | (super ? 8 : 0));
        }
        static Keys fromInt(Backing v) {
            Keys k;
            k.shift = (v & 1) != 0;
            k.ctrl = (v & 2) != 0;
            k.alt = (v & 4) != 0;
            k.super = (v & 8) != 0;
            return k;
        }
    };

    /* Tracks the side that is active for any given modifier. Note
     * that this doesn't confirm a modifier is pressed; you must check
     * the bool for that in addition to this.
     *
     * Not all platforms support this, check apprt for more info. */
    struct Side {
        typedef uint8_t Backing; /* u4 */

        ModSide shift = ModSide::left;
        ModSide ctrl = ModSide::left;
        ModSide alt = ModSide::left;
        ModSide super = ModSide::left;

        Backing int_() const {
            return (Backing)((uint8_t)shift | ((uint8_t)ctrl << 1) | ((uint8_t)alt << 2) | ((uint8_t)super << 3));
        }
        static Side fromInt(Backing v) {
            Side s;
            s.shift = (ModSide)(v & 1);
            s.ctrl = (ModSide)((v >> 1) & 1);
            s.alt = (ModSide)((v >> 2) & 1);
            s.super = (ModSide)((v >> 3) & 1);
            return s;
        }
    };

    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool super = false;
    bool caps_lock = false;
    bool num_lock = false;
    Side sides;
    uint8_t padding_ = 0; /* u6 */

    /* The mask that has all the side bits set. */
    static Mods side_mask() {
        Mods m;
        m.sides.shift = ModSide::right;
        m.sides.ctrl = ModSide::right;
        m.sides.alt = ModSide::right;
        m.sides.super = ModSide::right;
        return m;
    }

    /* Integer value of this struct. */
    Backing int_() const {
        return (Backing)((shift ? 1u : 0u) | (ctrl ? 2u : 0u) | (alt ? 4u : 0u) | (super ? 8u : 0u) |
                         (caps_lock ? 16u : 0u) | (num_lock ? 32u : 0u) | ((unsigned)sides.int_() << 6) |
                         ((unsigned)(padding_ & 0x3F) << 10));
    }

    /* Wisp: @bitCast(Backing) */
    static Mods fromInt(Backing v) {
        Mods m;
        m.shift = (v & 1) != 0;
        m.ctrl = (v & 2) != 0;
        m.alt = (v & 4) != 0;
        m.super = (v & 8) != 0;
        m.caps_lock = (v & 16) != 0;
        m.num_lock = (v & 32) != 0;
        m.sides = Side::fromInt((Side::Backing)((v >> 6) & 0xF));
        m.padding_ = (uint8_t)((v >> 10) & 0x3F);
        return m;
    }

    /* Wisp: expectEqual on the packed struct. */
    bool operator==(const Mods &o) const { return int_() == o.int_(); }
    bool operator!=(const Mods &o) const { return int_() != o.int_(); }

    /* Returns true if no modifiers are set. */
    bool empty() const { return int_() == 0; }

    /* Returns true if two mods are equal. */
    bool equal(const Mods &other) const { return int_() == other.int_(); }

    /* Returns only the keys.
     *
     * In the future I want to remove `binding` for this. I didn't want
     * to do that all in one PR where I added this because its a bigger
     * change. */
    Keys keys() const { return Keys::fromInt((Keys::Backing)(int_() & 0xF)); }

    /* Return mods that are only relevant for bindings. */
    Mods binding() const {
        Mods m;
        m.shift = shift;
        m.ctrl = ctrl;
        m.alt = alt;
        m.super = super;
        return m;
    }

    /* Perform `self &~ other` to remove the other mods from self. */
    Mods unset(const Mods &other) const { return fromInt((Backing)(int_() & ~other.int_())); }

    /* Returns the mods without locks set. */
    Mods withoutLocks() const {
        Mods copy = *this;
        copy.caps_lock = false;
        copy.num_lock = false;
        return copy;
    }

    /* Return the mods to use for key translation. This handles settings
     * like macos-option-as-alt. The translation mods should be used for
     * translation but never sent back in for the key callback.
     *
     * Wisp: the macos-option-as-alt block is Darwin-only; see the header. */
    Mods translation(OptionAsAlt option_as_alt) const {
        (void)option_as_alt;
        Mods result = *this;
        return result;
    }

    /* Checks to see if super is on (MacOS) or ctrl.
     *
     * Wisp: the non-Darwin branch; see the header. */
    bool ctrlOrSuper() const { return ctrl; }
};

/* Modifier remapping. See `key-remap` in Config.zig for detailed docs. */
struct RemapSet {
    /* Tracks which modifier keys and sides have remappings registered.
     * Used as a fast pre-check before doing expensive map lookups.
     *
     * The mask uses separate tracking for left and right sides because
     * remappings can be side-specific (e.g., only remap left_ctrl).
     *
     * Note: `left_sides` uses inverted logic where 1 means "left is remapped"
     * even though `Mod.Side.left = 0`. This allows efficient bitwise matching
     * since we can AND directly with the side bits. */
    struct Mask {
        /* Which modifier keys (shift/ctrl/alt/super) have any remapping. */
        Mods::Keys keys;
        /* Which modifiers have left-side remappings (inverted: 1 = left remapped). */
        Mods::Side left_sides;
        /* Which modifiers have right-side remappings (1 = right remapped). */
        Mods::Side right_sides;

        /* Adds a modifier to the mask, marking it as having a remapping. */
        void update(const Mods &mods) {
            const Mods::Keys::Backing keys_int = mods.keys().int_();

            /* OR the new keys into our existing keys mask.
             * Example: keys=0b0000, new ctrl → keys=0b0010 */
            keys = Mods::Keys::fromInt((Mods::Keys::Backing)(keys.int_() | keys_int));

            /* Both Keys and Side are u4 with matching bit positions.
             * This lets us use keys_int to select which side bits to update. */
            const Mods::Side::Backing sides = mods.sides.int_();
            const Mods::Side::Backing left_int = left_sides.int_();
            const Mods::Side::Backing right_int = right_sides.int_();

            /* Update left_sides: set bit if this key is active AND side is left.
             * Since Side.left=0, we invert sides (~sides) so left becomes 1.
             * keys_int masks to only affect the modifier being added.
             * Example: left_ctrl → keys_int=0b0010, ~sides=0b1111 (left=0 inverted)
             *          result: left_int | (0b0010 & 0b1111) = left_int | 0b0010 */
            left_sides = Mods::Side::fromInt((Mods::Side::Backing)((left_int | (keys_int & ~sides)) & 0xF));

            /* Update right_sides: set bit if this key is active AND side is right.
             * Since Side.right=1, we use sides directly.
             * Example: right_ctrl → keys_int=0b0010, sides=0b0010 (right=1)
             *          result: right_int | (0b0010 & 0b0010) = right_int | 0b0010 */
            right_sides = Mods::Side::fromInt((Mods::Side::Backing)((right_int | (keys_int & sides)) & 0xF));
        }

        /* Returns true if the given mods match any remapping in this mask.
         * This is a fast check to avoid expensive map lookups when no
         * remapping could possibly apply.
         *
         * Checks both that the modifier key is remapped AND that the
         * specific side (left/right) being pressed has a remapping. */
        bool match(const Mods &mods) const {
            /* Find which pressed keys have remappings registered.
             * Example: pressed={ctrl,alt}, mask={ctrl} → active=0b0010 (just ctrl) */
            const unsigned active = (unsigned)(mods.keys().int_() & keys.int_());
            if (active == 0) return false;

            /* Check if the pressed side matches a remapped side.
             * For left (sides bit = 0): check against left_int (where 1 = left remapped)
             *   ~sides inverts so left becomes 1, then AND with left_int
             * For right (sides bit = 1): check against right_int directly
             *
             * Example: pressing left_ctrl (sides.ctrl=0, left_int.ctrl=1)
             *   ~sides = 0b1111, left_int = 0b0010
             *   (~sides & left_int) = 0b0010 ✓ matches
             *
             * Example: pressing right_ctrl but only left_ctrl is remapped
             *   sides = 0b0010, left_int = 0b0010, right_int = 0b0000
             *   (~0b0010 & 0b0010) | (0b0010 & 0b0000) = 0b0000 ✗ no match */
            const unsigned sides = mods.sides.int_();
            const unsigned left_int = left_sides.int_();
            const unsigned right_int = right_sides.int_();
            const unsigned side_match = ((~sides & left_int) | (sides & right_int)) & 0xF;

            /* Final check: is any active (pressed + remapped) key also side-matched? */
            return (active & side_match) != 0;
        }
    };

    /* Wisp: std.AutoArrayHashMapUnmanaged(Mods, Mods); see the header. */
    struct Map {
        std::vector<std::pair<Mods, Mods>> entries;

        size_t count() const { return entries.size(); }
        void clearRetainingCapacity() { entries.clear(); }
        void put(const Mods &key, const Mods &value) {
            for (auto &e : entries) {
                if (e.first == key) {
                    e.second = value;
                    return;
                }
            }
            entries.push_back(std::make_pair(key, value));
        }
        bool swapRemove(const Mods &key) {
            for (size_t i = 0; i < entries.size(); i++) {
                if (entries[i].first == key) {
                    entries[i] = entries.back();
                    entries.pop_back();
                    return true;
                }
            }
            return false;
        }
        bool get(const Mods &key, Mods *out) const {
            for (const auto &e : entries) {
                if (e.first == key) {
                    *out = e.second;
                    return true;
                }
            }
            return false;
        }
    };

    /* Available mappings. */
    Map map;

    /* The mask of remapped modifiers that can be used to quickly
     * check if some input mods need remapping. */
    Mask mask;

    static RemapSet empty() { return RemapSet(); }

    enum class ParseError : uint8_t {
        none,
        OutOfMemory,
        MissingAssignment,
        InvalidMod,
    };

    /* Wisp: parseCLI's error set. */
    enum class CliError : uint8_t {
        none,
        OutOfMemory,
        InvalidValue,
    };

    /* Parse from CLI input. Required by Config.
     *
     * Wisp: `?[]const u8` is a pointer that may be null. */
    CliError parseCLI(const char *input) {
        const char *value = input ? input : "";

        /* Empty value resets the set */
        if (strlen(value) == 0) {
            map.clearRetainingCapacity();
            mask = Mask();
            return CliError::none;
        }

        switch (parse(value)) {
            case ParseError::none: return CliError::none;
            case ParseError::OutOfMemory: return CliError::OutOfMemory;
            case ParseError::MissingAssignment:
            case ParseError::InvalidMod: return CliError::InvalidValue;
        }
        return CliError::InvalidValue;
    }

    /* Parse a modifier remap and add it to the set. */
    ParseError parse(const std::string &input) {
        /* Find the assignment point ('=') */
        const size_t eql_idx = input.find('=');
        if (eql_idx == std::string::npos) return ParseError::MissingAssignment;

        /* The to side defaults to "left" if no explicit side is given.
         * This is because this is the default unsided value provided by
         * the apprts in the current Mods layout. */
        Mods to;
        {
            Mod mod;
            bool has_side = false;
            ModSide side = ModSide::left;
            if (!parseMod(input.substr(eql_idx + 1), &mod, &has_side, &side)) return ParseError::InvalidMod;
            to = initMods(mod, has_side ? side : ModSide::left);
        }

        /* The from side, if sided, is easy and we put it directly into
         * the map. */
        Mod from_mod;
        bool from_has_side = false;
        ModSide from_side = ModSide::left;
        if (!parseMod(input.substr(0, eql_idx), &from_mod, &from_has_side, &from_side)) {
            return ParseError::InvalidMod;
        }
        if (from_has_side) {
            const Mods from = initMods(from_mod, from_side);
            map.put(from, to);
            mask.update(from);
            return ParseError::none;
        }

        /* We need to do some combinatorial explosion here for unsided
         * from in order to assign all possible sides. */
        const Mods from_left = initMods(from_mod, ModSide::left);
        const Mods from_right = initMods(from_mod, ModSide::right);
        map.put(from_left, to);
        map.put(from_right, to);

        mask.update(from_left);
        mask.update(from_right);
        return ParseError::none;
    }

    void deinit() { map.entries.clear(); }

    /* Must be called prior to any remappings so that the mapping
     * is sorted properly. Otherwise, you will get invalid results. */
    void finalize() {
        /* Mods with any right sides prioritize */
        const Mods::Backing side_mask = Mods::side_mask().int_();
        std::stable_partition(map.entries.begin(), map.entries.end(),
                              [side_mask](const std::pair<Mods, Mods> &e) {
                                  return (e.first.int_() & side_mask) != 0;
                              });
    }

    /* Deep copy of the struct. Required by Config. */
    RemapSet clone() const {
        RemapSet result;
        result.map = map;
        result.mask = mask;
        return result;
    }

    /* Compare if two RemapSets are equal. Required by Config. */
    bool equal(const RemapSet &other) const {
        if (map.count() != other.map.count()) return false;

        for (const auto &entry : map.entries) {
            Mods other_value;
            if (!other.map.get(entry.first, &other_value)) return false;
            if (!entry.second.equal(other_value)) return false;
        }

        return true;
    }

    /* Used by Formatter. Required by Config. */
    template <typename Formatter>
    void formatEntry(Formatter formatter) const {
        if (map.count() == 0) {
            formatter.formatEntry(::wisp::datastruct::comparison::Void());
            return;
        }

        for (const auto &entry : map.entries) {
            const Mods from = entry.first;
            const Mods to = entry.second;

            std::string writer;
            formatMod(&writer, from);
            writer.push_back('=');
            formatMod(&writer, to);
            formatter.formatEntry(writer);
        }
    }

    static void formatMod(std::string *writer, const Mods &mods) {
        /* Check which mod is set and format it with optional side prefix */
        const struct {
            const char *name;
            bool set;
            ModSide side;
        } fields[] = {
            {"shift", mods.shift, mods.sides.shift},
            {"ctrl", mods.ctrl, mods.sides.ctrl},
            {"alt", mods.alt, mods.sides.alt},
            {"super", mods.super, mods.sides.super},
        };
        for (const auto &f : fields) {
            if (f.set) {
                if (f.side == ModSide::right) {
                    writer->append("right_");
                } else {
                    /* Only write left_ if we need to distinguish
                     * For now, always write left_ if it's a sided mapping */
                    writer->append("left_");
                }
                writer->append(f.name);
                return;
            }
        }
    }

    /* Parses a single mode in a single remapping string. E.g.
     * `ctrl` or `left_shift`.
     *
     * Wisp: `?Mod.Side` is `*has_side` plus `*side`; false is InvalidMod. */
    static bool parseMod(const std::string &input, Mod *mod_out, bool *has_side, ModSide *side_out) {
        std::string side_str;
        std::string mod_str;
        const size_t idx = input.find('_');
        if (idx != std::string::npos) {
            side_str = input.substr(0, idx);
            mod_str = input.substr(idx + 1);
        } else {
            mod_str = input;
        }

        Mod mod = Mod::shift;
        if (mod_str == "shift") {
            mod = Mod::shift;
        } else if (mod_str == "ctrl") {
            mod = Mod::ctrl;
        } else if (mod_str == "alt") {
            mod = Mod::alt;
        } else if (mod_str == "super") {
            mod = Mod::super;
        } else {
            bool found = false;
            for (const ModAlias &pair : alias) {
                if (mod_str == pair.name) {
                    mod = pair.mod;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }

        *has_side = false;
        if (side_str.size() > 0) {
            if (side_str == "left") {
                *side_out = ModSide::left;
            } else if (side_str == "right") {
                *side_out = ModSide::right;
            } else {
                return false;
            }
            *has_side = true;
        }
        *mod_out = mod;
        return true;
    }

    static Mods initMods(Mod mod, ModSide side) {
        Mods mods;
        switch (mod) {
            case Mod::shift:
                mods.shift = true;
                mods.sides.shift = side;
                break;
            case Mod::ctrl:
                mods.ctrl = true;
                mods.sides.ctrl = side;
                break;
            case Mod::alt:
                mods.alt = true;
                mods.sides.alt = side;
                break;
            case Mod::super:
                mods.super = true;
                mods.sides.super = side;
                break;
        }
        return mods;
    }

    /* Returns true if the given mods need remapping. */
    bool isRemapped(const Mods &mods) const { return mask.match(mods); }

    /* Apply a remap to the given mods. */
    Mods apply(const Mods &mods) const {
        if (!isRemapped(mods)) return mods;

        const unsigned mods_binding = mods.int_() & 0xF;
        const unsigned mods_sides = mods.sides.int_();

        for (const auto &entry : map.entries) {
            const Mods from = entry.first;
            const unsigned from_binding = from.int_() & 0xF;
            if ((mods_binding & from_binding) != from_binding) continue;
            const unsigned from_sides = from.sides.int_();
            if (((mods_sides ^ from_sides) & from_binding) != 0) continue;

            unsigned mods_int = mods.int_();
            mods_int &= ~(unsigned)from.int_();
            mods_int |= entry.second.int_();
            return Mods::fromInt((Mods::Backing)mods_int);
        }

        assert(false && "unreachable");
        return mods;
    }
};

} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KEY_MODS_HPP */
