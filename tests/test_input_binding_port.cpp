/* Transliterated from the test blocks in Ghostty src/input/Binding.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: Zig test-local struct literals become small builder helpers.
 */

#include <string>

#include "test_helpers.h"
#include "../src/config/formatter.hpp"
#include "../src/input/binding.hpp"
#include "../src/zigstd/allocator.hpp"

using namespace wisp::input;
using namespace wisp::input::binding;

TEST(input_binding, Flags_cval) {
    Flags f;
    ASSERT_EQ(0x1, (int)f.cval());
    f = Flags();
    f.consumed = false;
    ASSERT_EQ(0x0, (int)f.cval());
    f = Flags();
    f.all = true;
    ASSERT_EQ(0x3, (int)f.cval());
    f = Flags();
    f.global = true;
    ASSERT_EQ(0x5, (int)f.cval());
    f = Flags();
    f.performable = true;
    ASSERT_EQ(0x9, (int)f.cval());
    f = Flags();
    f.consumed = true;
    f.all = true;
    f.global = true;
    f.performable = true;
    ASSERT_EQ(0xF, (int)f.cval());
}

TEST(input_binding, SplitFocusDirection_parse) {
    typedef binding::Action::SplitFocusDirection D;
    D d;
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("previous", &d) == Error::none && d == D::previous);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("next", &d) == Error::none && d == D::next);

    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("up", &d) == Error::none && d == D::up);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("left", &d) == Error::none && d == D::left);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("down", &d) == Error::none && d == D::down);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("right", &d) == Error::none && d == D::right);

    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("top", &d) == Error::none && d == D::up);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("bottom", &d) == Error::none && d == D::down);

    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("", &d) == Error::InvalidFormat);
    ASSERT_TRUE(binding::Action::parseSplitFocusDirection("green", &d) == Error::InvalidFormat);
}

/* ---- helpers ---- */

typedef binding::Action BAction;
typedef BAction::Key AK;
typedef wisp::input::Key K;

static Mods mods(bool shift, bool ctrl, bool alt = false, bool super = false) {
    Mods m;
    m.shift = shift;
    m.ctrl = ctrl;
    m.alt = alt;
    m.super = super;
    return m;
}

static Trigger uni(uint32_t cp, Mods m = Mods()) {
    Trigger t;
    t.key = Trigger::Key::makeUnicode(cp);
    t.mods = m;
    return t;
}

static Trigger phys(K k, Mods m = Mods()) {
    Trigger t;
    t.key = Trigger::Key::makePhysical(k);
    t.mods = m;
    return t;
}

static Binding bind(const Trigger &t, AK a = AK::ignore, Flags f = Flags()) {
    Binding b;
    b.trigger = t;
    b.action = BAction::make(a);
    b.flags = f;
    return b;
}

static Flags flags(bool consumed, bool all, bool global, bool performable) {
    Flags f;
    f.consumed = consumed;
    f.all = all;
    f.global = global;
    f.performable = performable;
    return f;
}

/* Wisp: expectEqual(Binding, parseSingle(input)). */
static bool parsesTo(const char *input, const Binding &expected) {
    Binding b;
    if (parseSingle(input, &b) != Error::none) return false;
    return b.trigger.equal(expected.trigger) && b.action.equal(expected.action) && b.flags == expected.flags;
}

static Error parseErr(const char *input) {
    Binding b;
    return parseSingle(input, &b);
}

/* Wisp: `try parseSingle(input)`; a failure yields a `quit` action that
 * the following assertions reject. */
static Binding parsed(const char *input) {
    Binding b;
    if (parseSingle(input, &b) != Error::none) b.action.tag = AK::quit;
    return b;
}

TEST(input_binding, parse_triggers) {
    /* single character */
    ASSERT_TRUE(parsesTo("a=ignore", bind(uni('a'))));

    /* single modifier */
    ASSERT_TRUE(parsesTo("shift+a=ignore", bind(uni('a', mods(true, false)))));
    ASSERT_TRUE(parsesTo("ctrl+a=ignore", bind(uni('a', mods(false, true)))));

    /* multiple modifier */
    ASSERT_TRUE(parsesTo("shift+ctrl+a=ignore", bind(uni('a', mods(true, true)))));

    /* key can come before modifier */
    ASSERT_TRUE(parsesTo("a+shift=ignore", bind(uni('a', mods(true, false)))));

    /* physical keys */
    ASSERT_TRUE(parsesTo("shift+key_a=ignore", bind(phys(K::key_a, mods(true, false)))));

    /* unicode keys */
    ASSERT_TRUE(parsesTo("shift+\xc3\xb6=ignore", bind(uni(0xF6, mods(true, false)))));

    /* unconsumed keys */
    ASSERT_TRUE(parsesTo("unconsumed:shift+a=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(false, false, false, false))));

    /* unconsumed physical keys */
    ASSERT_TRUE(parsesTo("unconsumed:key_a+shift=ignore",
                         bind(phys(K::key_a, mods(true, false)), AK::ignore, flags(false, false, false, false))));

    /* performable keys */
    ASSERT_TRUE(parsesTo("performable:shift+a=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(true, false, false, true))));

    /* invalid key */
    ASSERT_TRUE(parseErr("foo=ignore") == Error::InvalidFormat);

    /* repeated control */
    ASSERT_TRUE(parseErr("shift+shift+a=ignore") == Error::InvalidFormat);

    /* multiple character */
    ASSERT_TRUE(parseErr("a+b=ignore") == Error::InvalidFormat);
}

TEST(input_binding, parse_w3c_key_names) {
    /* Exact match */
    ASSERT_TRUE(parsesTo("KeyA=ignore", bind(phys(K::key_a))));

    /* Case-sensitive */
    ASSERT_TRUE(parseErr("Keya=ignore") == Error::InvalidFormat);
}

TEST(input_binding, parse_catch_all) {
    Trigger t;
    t.key = Trigger::Key::makeCatchAll();

    /* Basic catch_all */
    ASSERT_TRUE(parsesTo("catch_all=ignore", bind(t)));

    /* catch_all with modifiers */
    t.mods = mods(false, true);
    ASSERT_TRUE(parsesTo("ctrl+catch_all=ignore", bind(t)));
}

TEST(input_binding, parse_plus_sign) {
    ASSERT_TRUE(parsesTo("+=ignore", bind(uni('+'))));

    /* Modifier */
    ASSERT_TRUE(parsesTo("ctrl++=ignore", bind(uni('+', mods(false, true)))));

    ASSERT_TRUE(parseErr("++=ignore") == Error::InvalidFormat);
}

TEST(input_binding, parse_equals_sign) {
    ASSERT_TRUE(parsesTo("==ignore", bind(uni('='))));

    /* Modifier */
    ASSERT_TRUE(parsesTo("ctrl+==ignore", bind(uni('=', mods(false, true)))));

    ASSERT_TRUE(parseErr("=ignore") == Error::InvalidFormat);
}

TEST(input_binding, parse_text_action_equals_sign) {
    {
        const Binding binding = parsed("==text:=");
        ASSERT_TRUE(uni('=').equal(binding.trigger));
        ASSERT_TRUE(binding.action.str == "=");
    }

    {
        const Binding binding = parsed("==text:=hello");
        ASSERT_TRUE(uni('=').equal(binding.trigger));
        ASSERT_TRUE(binding.action.str == "=hello");
    }

    {
        const Binding binding = parsed("ctrl+==text:=hello");
        ASSERT_TRUE(uni('=', mods(false, true)).equal(binding.trigger));
        ASSERT_TRUE(binding.action.str == "=hello");
    }

    {
        const Binding binding = parsed("=+ctrl=text:=hello");
        ASSERT_TRUE(uni('=', mods(false, true)).equal(binding.trigger));
        ASSERT_TRUE(binding.action.str == "=hello");
    }
}

/* For Ghostty 1.2+ we changed our key names to match the W3C and removed
 * `physical:`. This tests the backwards compatibility with the old format.
 * Note that our backwards compatibility isn't 100% perfect since triggers
 * like `a` now map to unicode instead of "translated" (which was also
 * removed). But we did our best here with what was unambiguous. */
TEST(input_binding, parse_backwards_compatibility_with_1_1_x) {
    /* simple, for sanity */
    ASSERT_TRUE(parsesTo("zero=ignore", bind(uni('0'))));
    ASSERT_TRUE(parsesTo("physical:zero=ignore", bind(phys(K::digit_0))));

    /* duplicates */
    ASSERT_TRUE(parseErr("zero+one=ignore") == Error::InvalidFormat);

    /* test our full map */
    size_t len = 0;
    const Trigger::BackwardsEntry *table = Trigger::backwardsCompatibleKeys(&len);
    for (size_t i = 0; i < len; i++) {
        Trigger t;
        t.key = table[i].value();
        const std::string input = std::string(table[i].name) + "=ignore";
        ASSERT_TRUE(parsesTo(input.c_str(), bind(t)));
    }
}

TEST(input_binding, parse_global_triggers) {
    /* global keys */
    ASSERT_TRUE(parsesTo("global:shift+a=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(true, false, true, false))));

    /* global physical keys */
    ASSERT_TRUE(parsesTo("global:key_a+shift=ignore",
                         bind(phys(K::key_a, mods(true, false)), AK::ignore, flags(true, false, true, false))));

    /* global unconsumed keys */
    ASSERT_TRUE(parsesTo("unconsumed:global:a+shift=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(false, false, true, false))));

    /* global sequences not allowed */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("global:a>b=ignore", &p) == Error::none);
        Parser::Elem e;
        bool has = false;
        ASSERT_TRUE(p.next(&e, &has) == Error::InvalidFormat);
    }
}

TEST(input_binding, parse_all_triggers) {
    /* all keys */
    ASSERT_TRUE(parsesTo("all:shift+a=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(true, true, false, false))));

    /* all physical keys */
    ASSERT_TRUE(parsesTo("all:key_a+shift=ignore",
                         bind(phys(K::key_a, mods(true, false)), AK::ignore, flags(true, true, false, false))));

    /* all unconsumed keys */
    ASSERT_TRUE(parsesTo("unconsumed:all:a+shift=ignore",
                         bind(uni('a', mods(true, false)), AK::ignore, flags(false, true, false, false))));

    /* all sequences not allowed */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("all:a>b=ignore", &p) == Error::none);
        Parser::Elem e;
        bool has = false;
        ASSERT_TRUE(p.next(&e, &has) == Error::InvalidFormat);
    }
}

TEST(input_binding, Trigger_equal) {
    /* Equal physical keys */
    ASSERT_TRUE(phys(K::arrow_up, mods(false, true)).equal(phys(K::arrow_up, mods(false, true))));

    /* Different physical keys */
    ASSERT_FALSE(phys(K::arrow_up, mods(false, true)).equal(phys(K::arrow_down, mods(false, true))));

    /* Different mods */
    ASSERT_FALSE(phys(K::arrow_up, mods(false, true)).equal(phys(K::arrow_up, mods(true, false))));

    /* Equal unicode keys */
    ASSERT_TRUE(uni('a').equal(uni('a')));

    /* Different unicode keys */
    ASSERT_FALSE(uni('a').equal(uni('b')));

    /* Different key types */
    ASSERT_FALSE(uni('a').equal(phys(K::key_a)));

    /* catch_all */
    Trigger t1;
    t1.key = Trigger::Key::makeCatchAll();
    Trigger t2 = t1;
    ASSERT_TRUE(t1.equal(t2));

    /* catch_all with different mods */
    t2.mods = mods(false, false, true);
    ASSERT_FALSE(t1.equal(t2));
}

TEST(input_binding, parse_modifier_aliases) {
    ASSERT_TRUE(parsesTo("cmd+a=ignore", bind(uni('a', mods(false, false, false, true)))));
    ASSERT_TRUE(parsesTo("command+a=ignore", bind(uni('a', mods(false, false, false, true)))));

    ASSERT_TRUE(parsesTo("opt+a=ignore", bind(uni('a', mods(false, false, true)))));
    ASSERT_TRUE(parsesTo("option+a=ignore", bind(uni('a', mods(false, false, true)))));

    ASSERT_TRUE(parsesTo("control+a=ignore", bind(uni('a', mods(false, true)))));
}

TEST(input_binding, parse_action_invalid) {
    /* invalid action */
    ASSERT_TRUE(parseErr("a=nopenopenope") == Error::InvalidAction);
}

TEST(input_binding, parse_action_no_parameters) {
    /* no parameters */
    ASSERT_TRUE(parsesTo("a=ignore", bind(uni('a'))));
    ASSERT_TRUE(parseErr("a=ignore:A") == Error::InvalidFormat);
}

TEST(input_binding, parse_action_with_string) {
    /* parameter */
    {
        const Binding binding = parsed("a=csi:A");
        ASSERT_TRUE(binding.action.tag == AK::csi);
        ASSERT_TRUE(binding.action.str == "A");
    }
    /* parameter */
    {
        const Binding binding = parsed("a=esc:A");
        ASSERT_TRUE(binding.action.tag == AK::esc);
        ASSERT_TRUE(binding.action.str == "A");
    }
    {
        const Binding binding = parsed("a=set_surface_title:surface");
        ASSERT_TRUE(binding.action.tag == AK::set_surface_title);
        ASSERT_TRUE(binding.action.str == "surface");
    }
    {
        const Binding binding = parsed("a=set_tab_title:tab");
        ASSERT_TRUE(binding.action.tag == AK::set_tab_title);
        ASSERT_TRUE(binding.action.str == "tab");
    }
}

TEST(input_binding, parse_action_with_enum) {
    /* parameter */
    const Binding binding = parsed("a=new_split:right");
    ASSERT_TRUE(binding.action.tag == AK::new_split);
    ASSERT_TRUE(BAction::SplitDirection::right == binding.action.split_direction);
}

TEST(input_binding, parse_action_with_enum_with_default) {
    /* parameter */
    const Binding binding = parsed("a=new_split");
    ASSERT_TRUE(binding.action.tag == AK::new_split);
    ASSERT_TRUE(BAction::SplitDirection::auto_ == binding.action.split_direction);
}

TEST(input_binding, parse_action_with_int) {
    /* parameter */
    {
        const Binding binding = parsed("a=jump_to_prompt:-1");
        ASSERT_TRUE(binding.action.tag == AK::jump_to_prompt);
        ASSERT_EQ(-1, (int)binding.action.i16);
    }
    {
        const Binding binding = parsed("a=jump_to_prompt:10");
        ASSERT_TRUE(binding.action.tag == AK::jump_to_prompt);
        ASSERT_EQ(10, (int)binding.action.i16);
    }
}

TEST(input_binding, parse_action_with_float) {
    /* parameter */
    {
        const Binding binding = parsed("a=scroll_page_fractional:-0.5");
        ASSERT_TRUE(binding.action.tag == AK::scroll_page_fractional);
        ASSERT_TRUE(-0.5f == binding.action.f32);
    }
    {
        const Binding binding = parsed("a=scroll_page_fractional:+0.5");
        ASSERT_TRUE(binding.action.tag == AK::scroll_page_fractional);
        ASSERT_TRUE(0.5f == binding.action.f32);
    }
}

TEST(input_binding, parse_action_with_a_tuple) {
    /* parameter */
    {
        const Binding binding = parsed("a=resize_split:up,10");
        ASSERT_TRUE(binding.action.tag == AK::resize_split);
        ASSERT_TRUE(BAction::SplitResizeDirection::up == binding.action.split_resize_parameter.direction);
        ASSERT_EQ(10, (int)binding.action.split_resize_parameter.amount);
    }

    /* missing parameter */
    ASSERT_TRUE(parseErr("a=resize_split:up") == Error::InvalidFormat);

    /* too many */
    ASSERT_TRUE(parseErr("a=resize_split:up,10,12") == Error::InvalidFormat);

    /* invalid type */
    ASSERT_TRUE(parseErr("a=resize_split:up,four") == Error::InvalidFormat);
}

TEST(input_binding, parse_chain) {
    /* Valid */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("chain=new_tab", &p) == Error::none);
        Parser::Elem e;
        bool has = false;
        ASSERT_TRUE(p.next(&e, &has) == Error::none);
        ASSERT_TRUE(has);
        ASSERT_TRUE(e.tag == Parser::Elem::Tag::chain);
        ASSERT_TRUE(e.chain.equal(BAction::make(AK::new_tab)));
        ASSERT_TRUE(p.next(&e, &has) == Error::none);
        ASSERT_FALSE(has);
    }

    /* Chain can't have flags */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("global:chain=ignore", &p) == Error::InvalidFormat);
    }

    /* Chain can't be part of a sequence */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("a>chain=ignore", &p) == Error::none);
        Parser::Elem e;
        bool has = false;
        ASSERT_TRUE(p.next(&e, &has) == Error::none);
        ASSERT_TRUE(p.next(&e, &has) == Error::InvalidFormat);
    }
}

TEST(input_binding, sequence_iterator) {
    Trigger t;
    bool has = false;

    /* single character */
    {
        SequenceIterator it;
        it.input = "a";
        ASSERT_TRUE(it.next(&t, &has) == Error::none && has);
        ASSERT_TRUE(uni('a').equal(t));
        ASSERT_TRUE(it.next(&t, &has) == Error::none);
        ASSERT_FALSE(has);
    }

    /* multi character */
    {
        SequenceIterator it;
        it.input = "a>b";
        ASSERT_TRUE(it.next(&t, &has) == Error::none && has);
        ASSERT_TRUE(uni('a').equal(t));
        ASSERT_TRUE(it.next(&t, &has) == Error::none && has);
        ASSERT_TRUE(uni('b').equal(t));
        ASSERT_TRUE(it.next(&t, &has) == Error::none);
        ASSERT_FALSE(has);
    }

    /* empty */
    {
        SequenceIterator it;
        it.input = "";
        ASSERT_TRUE(it.next(&t, &has) == Error::InvalidFormat);
    }

    /* empty starting sequence */
    {
        SequenceIterator it;
        it.input = ">a";
        ASSERT_TRUE(it.next(&t, &has) == Error::InvalidFormat);
    }

    /* empty ending sequence */
    {
        SequenceIterator it;
        it.input = "a>";
        ASSERT_TRUE(it.next(&t, &has) == Error::none && has);
        ASSERT_TRUE(uni('a').equal(t));
        ASSERT_TRUE(it.next(&t, &has) == Error::InvalidFormat);
    }
}

TEST(input_binding, parse_sequences) {
    Parser::Elem e;
    bool has = false;

    /* single character */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("ctrl+a=ignore", &p) == Error::none);
        ASSERT_TRUE(p.next(&e, &has) == Error::none && has);
        ASSERT_TRUE(e.tag == Parser::Elem::Tag::binding);
        ASSERT_TRUE(e.binding.trigger.equal(uni('a', mods(false, true))));
        ASSERT_TRUE(e.binding.action.equal(BAction::make(AK::ignore)));
        ASSERT_TRUE(e.binding.flags == Flags());
        ASSERT_TRUE(p.next(&e, &has) == Error::none);
        ASSERT_FALSE(has);
    }

    /* sequence */
    {
        Parser p;
        ASSERT_TRUE(Parser::init("a>b=ignore", &p) == Error::none);
        ASSERT_TRUE(p.next(&e, &has) == Error::none && has);
        ASSERT_TRUE(e.tag == Parser::Elem::Tag::leader);
        ASSERT_TRUE(e.leader.equal(uni('a')));
        ASSERT_TRUE(p.next(&e, &has) == Error::none && has);
        ASSERT_TRUE(e.tag == Parser::Elem::Tag::binding);
        ASSERT_TRUE(e.binding.trigger.equal(uni('b')));
        ASSERT_TRUE(e.binding.action.equal(BAction::make(AK::ignore)));
        ASSERT_TRUE(p.next(&e, &has) == Error::none);
        ASSERT_FALSE(has);
    }
}

TEST(input_binding, Action_clone) {
    {
        BAction a = BAction::make(AK::ignore);
        const BAction b = a.clone();
        ASSERT_TRUE(b.tag == AK::ignore);
    }

    {
        BAction a = BAction::makeStr(AK::text, "foo");
        const BAction b = a.clone();
        ASSERT_TRUE(b.tag == AK::text);
    }
}

TEST(input_binding, parse_increase_font_size) {
    const Binding binding = parsed("a=increase_font_size:1.5");
    ASSERT_TRUE(binding.action.tag == AK::increase_font_size);
    ASSERT_TRUE(1.5f == binding.action.f32);
}

TEST(input_binding, parse_decrease_font_size) {
    const Binding binding = parsed("a=decrease_font_size:2.5");
    ASSERT_TRUE(binding.action.tag == AK::decrease_font_size);
    ASSERT_TRUE(2.5f == binding.action.f32);
}

TEST(input_binding, parse_reset_font_size) {
    const Binding binding = parsed("a=reset_font_size");
    ASSERT_TRUE(binding.action.tag == AK::reset_font_size);
}

TEST(input_binding, parse_set_font_size) {
    const Binding binding = parsed("a=set_font_size:13.5");
    ASSERT_TRUE(binding.action.tag == AK::set_font_size);
    ASSERT_TRUE(13.5f == binding.action.f32);
}

TEST(input_binding, parse_copy_to_clipboard_default) {
    const Binding binding = parsed("a=copy_to_clipboard");
    ASSERT_TRUE(binding.action.tag == AK::copy_to_clipboard);
    ASSERT_TRUE(BAction::CopyToClipboard::mixed == binding.action.copy_to_clipboard);
}

TEST(input_binding, parse_copy_to_clipboard_explicit) {
    const Binding binding = parsed("a=copy_to_clipboard:html");
    ASSERT_TRUE(binding.action.tag == AK::copy_to_clipboard);
    ASSERT_TRUE(BAction::CopyToClipboard::html == binding.action.copy_to_clipboard);
}

TEST(input_binding, parse_write_screen_file_no_format) {
    const Binding binding = parsed("a=write_screen_file:copy");
    ASSERT_TRUE(binding.action.tag == AK::write_screen_file);
    ASSERT_TRUE(wisp::datastruct::comparison::deepEqual(BAction::WriteScreen::copy(), binding.action.write_screen));
}

TEST(input_binding, parse_write_screen_file_format) {
    const Binding binding = parsed("a=write_screen_file:copy,html");
    ASSERT_TRUE(binding.action.tag == AK::write_screen_file);
    const BAction::WriteScreen expected = {BAction::WriteScreen::Action::copy, BAction::WriteScreen::Format::html};
    ASSERT_TRUE(wisp::datastruct::comparison::deepEqual(expected, binding.action.write_screen));
}

TEST(input_binding, parse_write_screen_file_format_as_string) {
    const Binding binding = parsed("a=write_screen_file:copy,html");
    ASSERT_TRUE(binding.action.format() == "write_screen_file:copy,html");
}

TEST(input_binding, parse_write_screen_file_invalid) {
    /* paramet  r */
    ASSERT_TRUE(parseErr("a=write_screen_file:") == Error::InvalidFormat);
    ASSERT_TRUE(parseErr("a=write_screen_file:,") == Error::InvalidFormat);
    ASSERT_TRUE(parseErr("a=write_screen_file:copy,") == Error::InvalidFormat);
    ASSERT_TRUE(parseErr("a=write_screen_file:copy,html,extra") == Error::InvalidFormat);
}

TEST(input_binding, action_format) {
    const BAction a = BAction::makeStr(AK::text, "\xf0\x9f\x91\xbbGhostty'\"");

    const std::string buf = a.format();

    BAction b;
    ASSERT_TRUE(BAction::parse(buf, &b) == Error::none);
    ASSERT_TRUE(a.equal(b));
}

TEST(input_binding, action_format_set_title) {
    const BAction a = BAction::makeStr(AK::set_tab_title, "foo bar");

    ASSERT_TRUE(a.format() == "set_tab_title:foo bar");
}

/* ---- Set ---- */

/* Wisp: `defer s.deinit(alloc)`. */
struct SetHolder {
    Set s;
    ~SetHolder() { s.deinit(); }
};

static bool chainKeyIs(const Set &s, const char *expected) {
    return s.has_chain_parent && s.chain_parent.key_ptr()->format() == expected;
}

TEST(input_binding, set_parseAndPut_typical_binding) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a=new_window") == Error::none);

    /* Creates forward mapping */
    {
        const Set::Entry *e = s.get(uni('a'));
        ASSERT_TRUE(e != nullptr);
        ASSERT_TRUE(e->value.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(Flags() == e->value.leaf.flags);
    }

    /* Creates reverse mapping */
    {
        Trigger trigger;
        ASSERT_TRUE(s.getTrigger(BAction::make(AK::new_window), &trigger));
        ASSERT_TRUE(trigger.key.unicode == 'a');
    }

    /* Sets up the chain parent properly */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "a"));
}

TEST(input_binding, set_parseAndPut_unconsumed_binding) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("unconsumed:a=new_window") == Error::none);

    /* Creates forward mapping */
    {
        const Set::Entry *e = s.get(uni('a'));
        ASSERT_TRUE(e != nullptr);
        ASSERT_TRUE(e->value.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(flags(false, false, false, false) == e->value.leaf.flags);
    }

    /* Creates reverse mapping */
    {
        Trigger trigger;
        ASSERT_TRUE(s.getTrigger(BAction::make(AK::new_window), &trigger));
        ASSERT_TRUE(trigger.key.unicode == 'a');
    }

    /* Sets up the chain parent properly */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "a"));
}

TEST(input_binding, set_parseAndPut_removed_binding) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(s.parseAndPut("a=unbind") == Error::none);

    /* Creates forward mapping */
    ASSERT_TRUE(s.get(uni('a')) == nullptr);
    Trigger trigger;
    ASSERT_FALSE(s.getTrigger(BAction::make(AK::new_window), &trigger));

    /* Sets up the chain parent properly */
    ASSERT_FALSE(s.has_chain_parent);
}

TEST(input_binding, set_put_sets_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    s.put(uni('a'), BAction::make(AK::new_window));

    /* chain_parent should be set */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "a"));

    /* chain_parent value should be a leaf */
    ASSERT_TRUE(s.chain_parent.value_ptr()->tag == Set::Value::Tag::leaf);
}

TEST(input_binding, set_putFlags_sets_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    s.putFlags(uni('a'), BAction::make(AK::new_window), flags(false, false, false, false));

    /* chain_parent should be set */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "a"));

    /* chain_parent value should be a leaf with correct flags */
    ASSERT_TRUE(s.chain_parent.value_ptr()->tag == Set::Value::Tag::leaf);
    ASSERT_FALSE(s.chain_parent.value_ptr()->leaf.flags.consumed);
}

TEST(input_binding, set_sequence_sets_chain_parent_to_final_leaf) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a>b=new_window") == Error::none);

    /* chain_parent should be set and point to 'b' (the final leaf) */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "b"));

    /* chain_parent value should be a leaf */
    ASSERT_TRUE(s.chain_parent.value_ptr()->tag == Set::Value::Tag::leaf);
    ASSERT_TRUE(s.chain_parent.value_ptr()->leaf.action.tag == AK::new_window);
}

TEST(input_binding, set_multiple_leaves_under_leader_updates_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a>b=new_window") == Error::none);

    /* After first binding, chain_parent should be 'b' */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "b"));

    ASSERT_TRUE(s.parseAndPut("a>c=new_tab") == Error::none);

    /* After second binding, chain_parent should be updated to 'c' */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(chainKeyIs(s, "c"));
    ASSERT_TRUE(s.chain_parent.value_ptr()->leaf.action.tag == AK::new_tab);
}

TEST(input_binding, set_sequence_unbind_clears_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a>b=new_window") == Error::none);
    ASSERT_TRUE(s.has_chain_parent);

    ASSERT_TRUE(s.parseAndPut("a>b=unbind") == Error::none);

    /* After unbind, chain_parent should be cleared */
    ASSERT_FALSE(s.has_chain_parent);
}

TEST(input_binding, set_sequence_unbind_with_remaining_leaves_clears_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a>b=new_window") == Error::none);
    ASSERT_TRUE(s.parseAndPut("a>c=new_tab") == Error::none);
    ASSERT_TRUE(s.parseAndPut("a>b=unbind") == Error::none);

    /* After unbind, chain_parent should be cleared even though 'c' remains */
    ASSERT_FALSE(s.has_chain_parent);

    /* But 'c' should still exist */
    const Set::Entry *a_entry = s.get(uni('a'));
    ASSERT_TRUE(a_entry != nullptr);
    ASSERT_TRUE(a_entry->value.tag == Set::Value::Tag::leader);
    const Set *inner_set = a_entry->value.leader;
    ASSERT_TRUE(inner_set->get(uni('c')) != nullptr);
}

TEST(input_binding, set_direct_remove_clears_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(s.has_chain_parent);

    s.remove(uni('a'));

    /* After removal, chain_parent should be cleared */
    ASSERT_FALSE(s.has_chain_parent);
}

TEST(input_binding, set_invalid_format_preserves_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a=new_window") == Error::none);
    const Trigger *before_key = s.chain_parent.key_ptr();
    const Set::Value *before_value = s.chain_parent.value_ptr();

    /* Try an invalid parse - should fail */
    ASSERT_TRUE(s.parseAndPut("a=invalid_action_xyz") == Error::InvalidAction);

    /* chain_parent should be unchanged */
    ASSERT_TRUE(s.has_chain_parent);
    ASSERT_TRUE(s.chain_parent.key_ptr() == before_key);
    ASSERT_TRUE(s.chain_parent.value_ptr() == before_value);
}

TEST(input_binding, set_clone_produces_null_chain_parent) {
    SetHolder h;
    Set &s = h.s;

    ASSERT_TRUE(s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(s.has_chain_parent);

    SetHolder cloned;
    cloned.s = s.clone();

    /* Clone should have null chain_parent */
    ASSERT_FALSE(cloned.s.has_chain_parent);

    /* But should have the binding */
    ASSERT_TRUE(cloned.s.get(uni('a')) != nullptr);
}

TEST(input_binding, set_clone_with_leaf_chained) {
    SetHolder h;
    Set &s = h.s;

    /* Create a chained binding using parseAndPut with chain= */
    ASSERT_TRUE(s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(s.parseAndPut("chain=new_tab") == Error::none);

    /* Verify we have a leaf_chained */
    const Set::Entry *entry = s.get(uni('a'));
    ASSERT_TRUE(entry != nullptr);
    ASSERT_TRUE(entry->value.tag == Set::Value::Tag::leaf_chained);
    ASSERT_EQ(2L, (long)entry->value.leaf_chained.actions.size());

    /* Clone the set */
    SetHolder cloned;
    cloned.s = s.clone();

    /* Verify the cloned set has the leaf_chained with same actions */
    const Set::Entry *cloned_entry = cloned.s.get(uni('a'));
    ASSERT_TRUE(cloned_entry != nullptr);
    ASSERT_TRUE(cloned_entry->value.tag == Set::Value::Tag::leaf_chained);
    ASSERT_EQ(2L, (long)cloned_entry->value.leaf_chained.actions.size());
    ASSERT_TRUE(cloned_entry->value.leaf_chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(cloned_entry->value.leaf_chained.actions[1].tag == AK::new_tab);
}

TEST(input_binding, set_clone_with_leaf_chained_containing_allocated_data) {
    SetHolder h;
    Set &s = h.s;

    /* Create a chained binding with text actions (which have allocated strings) */
    ASSERT_TRUE(s.parseAndPut("a=text:hello") == Error::none);
    ASSERT_TRUE(s.parseAndPut("chain=text:world") == Error::none);

    /* Verify we have a leaf_chained */
    const Set::Entry *entry = s.get(uni('a'));
    ASSERT_TRUE(entry != nullptr);
    ASSERT_TRUE(entry->value.tag == Set::Value::Tag::leaf_chained);

    /* Clone the set */
    SetHolder cloned;
    cloned.s = s.clone();

    /* Verify the cloned set has independent copies of the text */
    const Set::Entry *cloned_entry = cloned.s.get(uni('a'));
    ASSERT_TRUE(cloned_entry != nullptr);
    ASSERT_TRUE(cloned_entry->value.tag == Set::Value::Tag::leaf_chained);
    ASSERT_TRUE(cloned_entry->value.leaf_chained.actions[0].str == "hello");
    ASSERT_TRUE(cloned_entry->value.leaf_chained.actions[1].str == "world");

    /* Verify the pointers are different (truly cloned, not shared) */
    ASSERT_TRUE(entry->value.leaf_chained.actions[0].str.data() !=
                cloned_entry->value.leaf_chained.actions[0].str.data());
}

/* Wisp: `s.get(t).?.value_ptr.*` (asserting presence). */
static Set::Value valueOf(const Set &s, const Trigger &t) {
    const Set::Entry *e = s.get(t);
    if (e == nullptr) {
        Set::Value missing;
        missing.tag = Set::Value::Tag::leaf;
        missing.leaf.action = BAction::make(AK::quit);
        return missing;
    }
    return e->value;
}

static bool reverseIs(const Set &s, AK action, uint32_t cp) {
    Trigger trigger;
    return s.getTrigger(BAction::make(action), &trigger) && trigger.key.unicode == cp;
}

static bool noReverse(const Set &s, AK action) {
    Trigger trigger;
    return !s.getTrigger(BAction::make(action), &trigger);
}

TEST(input_binding, set_parseAndPut_sequence) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    const Set *current = &h.s;
    {
        const Set::Value e = valueOf(*current, uni('a'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leader);
        current = e.leader;
    }
    {
        const Set::Value e = valueOf(*current, uni('b'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leaf);
        ASSERT_TRUE(e.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(Flags() == e.leaf.flags);
    }
}

TEST(input_binding, set_parseAndPut_sequence_with_two_actions) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("a>c=new_tab") == Error::none);
    const Set *current = &h.s;
    {
        const Set::Value e = valueOf(*current, uni('a'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leader);
        current = e.leader;
    }
    {
        const Set::Value e = valueOf(*current, uni('b'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leaf);
        ASSERT_TRUE(e.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(Flags() == e.leaf.flags);
    }
    {
        const Set::Value e = valueOf(*current, uni('c'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leaf);
        ASSERT_TRUE(e.leaf.action.tag == AK::new_tab);
        ASSERT_TRUE(Flags() == e.leaf.flags);
    }
}

TEST(input_binding, set_parseAndPut_overwrite_sequence) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_tab") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    const Set *current = &h.s;
    {
        const Set::Value e = valueOf(*current, uni('a'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leader);
        current = e.leader;
    }
    {
        const Set::Value e = valueOf(*current, uni('b'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leaf);
        ASSERT_TRUE(e.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(Flags() == e.leaf.flags);
    }
}

TEST(input_binding, set_parseAndPut_overwrite_leader) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_tab") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    const Set *current = &h.s;
    {
        const Set::Value e = valueOf(*current, uni('a'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leader);
        current = e.leader;
    }
    {
        const Set::Value e = valueOf(*current, uni('b'));
        ASSERT_TRUE(e.tag == Set::Value::Tag::leaf);
        ASSERT_TRUE(e.leaf.action.tag == AK::new_window);
        ASSERT_TRUE(Flags() == e.leaf.flags);
    }
}

TEST(input_binding, set_parseAndPut_unbind_sequence_unbinds_leader) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("a>b=unbind") == Error::none);
    ASSERT_TRUE(h.s.get(uni('a')) == nullptr);
}

TEST(input_binding, set_parseAndPut_unbind_sequence_unbinds_leader_if_not_set) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=unbind") == Error::none);
    ASSERT_TRUE(h.s.get(uni('a')) == nullptr);
}

TEST(input_binding, set_parseAndPut_sequence_preserves_reverse_mapping) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("ctrl+a>b=new_window") == Error::none);

    /* Creates reverse mapping */
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));
}

TEST(input_binding, set_put_overwrites_sequence) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("ctrl+a>b=new_window") == Error::none);
    h.s.put(uni('a', mods(false, true)), BAction::make(AK::new_window));

    /* Creates reverse mapping */
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));
}

TEST(input_binding, set_maintains_reverse_mapping) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));

    /* should be most recent */
    h.s.put(uni('b'), BAction::make(AK::new_window));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'b'));

    /* removal should replace */
    h.s.remove(uni('b'));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));
}

TEST(input_binding, set_performable_is_not_part_of_reverse_mappings) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));

    /* trigger should be non-performable */
    h.s.putFlags(uni('b'), BAction::make(AK::new_window), flags(true, false, false, true));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));

    /* removal of performable should do nothing */
    h.s.remove(uni('b'));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));
}

TEST(input_binding, set_overriding_a_mapping_updates_reverse) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));

    /* should be most recent */
    h.s.put(uni('a'), BAction::make(AK::new_tab));
    ASSERT_TRUE(noReverse(h.s, AK::new_window));
}

TEST(input_binding, set_consumed_state) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf);
    ASSERT_TRUE(valueOf(h.s, uni('a')).leaf.flags.consumed);

    h.s.putFlags(uni('a'), BAction::make(AK::new_window), flags(false, false, false, false));
    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf);
    ASSERT_FALSE(valueOf(h.s, uni('a')).leaf.flags.consumed);

    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf);
    ASSERT_TRUE(valueOf(h.s, uni('a')).leaf.flags.consumed);
}

TEST(input_binding, set_parseAndPut_chain) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);

    /* Creates forward mapping as leaf_chained */
    {
        const Set::Value entry = valueOf(h.s, uni('a'));
        ASSERT_TRUE(entry.tag == Set::Value::Tag::leaf_chained);
        const Set::LeafChained &chained = entry.leaf_chained;
        ASSERT_EQ(2L, (long)chained.actions.size());
        ASSERT_TRUE(chained.actions[0].tag == AK::new_window);
        ASSERT_TRUE(chained.actions[1].tag == AK::new_tab);
    }

    /* Does not create reverse mapping, because reverse mappings are only for
     * non-chained actions. */
    ASSERT_TRUE(noReverse(h.s, AK::new_window));
}

TEST(input_binding, set_parseAndPut_chain_without_parent_is_error) {
    SetHolder h;

    /* Chain without a prior binding should fail */
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::InvalidFormat);
}

TEST(input_binding, set_parseAndPut_chain_multiple_times) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=close_surface") == Error::none);

    /* Should have 3 actions chained */
    const Set::Value entry = valueOf(h.s, uni('a'));
    ASSERT_TRUE(entry.tag == Set::Value::Tag::leaf_chained);
    const Set::LeafChained &chained = entry.leaf_chained;
    ASSERT_EQ(3L, (long)chained.actions.size());
    ASSERT_TRUE(chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(chained.actions[1].tag == AK::new_tab);
    ASSERT_TRUE(chained.actions[2].tag == AK::close_surface);
}

TEST(input_binding, set_parseAndPut_chain_preserves_flags) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("unconsumed:a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);

    /* Should preserve unconsumed flag */
    const Set::Value entry = valueOf(h.s, uni('a'));
    ASSERT_TRUE(entry.tag == Set::Value::Tag::leaf_chained);
    ASSERT_FALSE(entry.leaf_chained.flags.consumed);
    ASSERT_EQ(2L, (long)entry.leaf_chained.actions.size());
}

TEST(input_binding, set_parseAndPut_chain_after_unbind_is_error) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("a=unbind") == Error::none);

    /* Chain after unbind should fail because chain_parent is cleared */
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::InvalidFormat);
}

TEST(input_binding, set_parseAndPut_chain_on_sequence) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a>b=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);

    /* Navigate to the inner set */
    const Set::Value a_entry = valueOf(h.s, uni('a'));
    ASSERT_TRUE(a_entry.tag == Set::Value::Tag::leader);
    const Set *inner_set = a_entry.leader;

    /* Check the chained binding */
    const Set::Value b_entry = valueOf(*inner_set, uni('b'));
    ASSERT_TRUE(b_entry.tag == Set::Value::Tag::leaf_chained);
    ASSERT_EQ(2L, (long)b_entry.leaf_chained.actions.size());
    ASSERT_TRUE(b_entry.leaf_chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(b_entry.leaf_chained.actions[1].tag == AK::new_tab);
}

TEST(input_binding, set_parseAndPut_chain_with_unbind_is_error) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);

    /* chain=unbind is not valid */
    ASSERT_TRUE(h.s.parseAndPut("chain=unbind") == Error::InvalidFormat);

    /* Original binding should still exist */
    const Set::Value entry = valueOf(h.s, uni('a'));
    ASSERT_TRUE(entry.tag == Set::Value::Tag::leaf);
    ASSERT_TRUE(entry.leaf.action.tag == AK::new_window);
}

/* Wisp: KeyEvent literal for getEvent tests. */
static KeyEvent keyEvent(K k, Mods m, const char *utf8 = "", uint32_t unshifted = 0) {
    KeyEvent e;
    e.key = k;
    e.mods = m;
    e.utf8 = utf8;
    e.utf8_len = strlen(utf8);
    e.unshifted_codepoint = unshifted;
    return e;
}

/* Wisp: `s.getEvent(ev).?.value_ptr.*.leaf.action == a`. */
static bool eventIs(const Set &s, const KeyEvent &ev, AK a) {
    const Set::Entry *e = s.getEvent(ev);
    return e != nullptr && e->value.tag == Set::Value::Tag::leaf && e->value.leaf.action.tag == a;
}

TEST(input_binding, set_getEvent_physical) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("ctrl+quote=new_window") == Error::none);

    /* Physical matches on physical */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::quote, mods(false, true)), AK::new_window));

    /* Physical does not match on UTF8/codepoint */
    ASSERT_TRUE(h.s.getEvent(keyEvent(K::key_a, mods(false, true), "'", '\'')) == nullptr);
}

TEST(input_binding, set_getEvent_codepoint) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("ctrl+'=new_window") == Error::none);

    /* Matches on codepoint */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, true), "", '\''), AK::new_window));

    /* Matches on UTF-8 */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, true), "'"), AK::new_window));

    /* Doesn't match on physical */
    ASSERT_TRUE(h.s.getEvent(keyEvent(K::key_a, mods(false, true))) == nullptr);
}

TEST(input_binding, set_getEvent_codepoint_case_folding) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("ctrl+A=new_window") == Error::none);

    /* Lowercase codepoint */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_j, mods(false, true), "", 'a'), AK::new_window));

    /* Uppercase codepoint */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, true), "", 'A'), AK::new_window));

    /* Negative case for sanity */
    ASSERT_TRUE(h.s.getEvent(keyEvent(K::key_j, mods(false, true))) == nullptr);
}

TEST(input_binding, set_getEvent_catch_all_fallback) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("catch_all=ignore") == Error::none);

    /* Matches unbound key without modifiers */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, Mods()), AK::ignore));

    /* Matches unbound key with modifiers (falls back to catch_all without mods) */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, true)), AK::ignore));

    /* Specific binding takes precedence over catch_all */
    ASSERT_TRUE(h.s.parseAndPut("ctrl+b=new_window") == Error::none);
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_b, mods(false, true), "", 'b'), AK::new_window));
}

TEST(input_binding, set_getEvent_catch_all_with_modifiers) {
    SetHolder h;
    ASSERT_TRUE(h.s.parseAndPut("ctrl+catch_all=close_surface") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("catch_all=ignore") == Error::none);

    /* Key with ctrl matches catch_all with ctrl modifier */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, true)), AK::close_surface));

    /* Key without mods matches catch_all without mods */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, Mods()), AK::ignore));

    /* Key with different mods falls back to catch_all without mods */
    ASSERT_TRUE(eventIs(h.s, keyEvent(K::key_a, mods(false, false, true)), AK::ignore));
}

static Set::LeafChained chainedOf(const Set &s, const Trigger &t) {
    const Set::Entry *e = s.get(t);
    if (e == nullptr || e->value.tag != Set::Value::Tag::leaf_chained) return Set::LeafChained();
    return e->value.leaf_chained;
}

TEST(input_binding, set_appendChain_with_no_parent_returns_error) {
    SetHolder h;

    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::NoChainParent);
}

TEST(input_binding, set_appendChain_after_put_converts_to_leaf_chained) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));

    /* First appendChain converts leaf to leaf_chained and appends the new action */
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);

    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf_chained);

    const Set::LeafChained chained = chainedOf(h.s, uni('a'));
    ASSERT_EQ(2L, (long)chained.actions.size());
    ASSERT_TRUE(chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(chained.actions[1].tag == AK::new_tab);
}

TEST(input_binding, set_appendChain_after_putFlags_preserves_flags) {
    SetHolder h;
    h.s.putFlags(uni('a'), BAction::make(AK::new_window), flags(false, false, false, false));
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);

    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf_chained);

    const Set::LeafChained chained = chainedOf(h.s, uni('a'));
    ASSERT_FALSE(chained.flags.consumed);
    ASSERT_EQ(2L, (long)chained.actions.size());
    ASSERT_TRUE(chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(chained.actions[1].tag == AK::new_tab);
}

TEST(input_binding, set_appendChain_multiple_times) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::close_surface)) == Set::RecurseError::none);

    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf_chained);

    const Set::LeafChained chained = chainedOf(h.s, uni('a'));
    ASSERT_EQ(3L, (long)chained.actions.size());
    ASSERT_TRUE(chained.actions[0].tag == AK::new_window);
    ASSERT_TRUE(chained.actions[1].tag == AK::new_tab);
    ASSERT_TRUE(chained.actions[2].tag == AK::close_surface);
}

TEST(input_binding, set_appendChain_removes_reverse_mapping) {
    SetHolder h;
    h.s.put(uni('a'), BAction::make(AK::new_window));

    /* Verify reverse mapping exists before chaining */
    ASSERT_FALSE(noReverse(h.s, AK::new_window));

    /* Chaining should remove the reverse mapping */
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);

    /* Reverse mapping should be gone since chained actions are not in reverse map */
    ASSERT_TRUE(noReverse(h.s, AK::new_window));
}

TEST(input_binding, set_appendChain_with_performable_does_not_affect_reverse_mapping) {
    SetHolder h;

    /* Add a non-performable binding first */
    h.s.put(uni('b'), BAction::make(AK::new_window));
    ASSERT_FALSE(noReverse(h.s, AK::new_window));

    /* Add a performable binding (not in reverse map) and chain it */
    h.s.putFlags(uni('a'), BAction::make(AK::close_surface), flags(true, false, false, true));

    /* close_surface was performable, so not in reverse map */
    ASSERT_TRUE(noReverse(h.s, AK::close_surface));

    /* Chaining the performable binding should not crash or affect anything */
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);

    /* The non-performable new_window binding should still be in reverse map */
    ASSERT_FALSE(noReverse(h.s, AK::new_window));
}

TEST(input_binding, set_appendChain_restores_next_valid_reverse_mapping) {
    SetHolder h;

    /* Add two bindings for the same action */
    h.s.put(uni('a'), BAction::make(AK::new_window));
    h.s.put(uni('b'), BAction::make(AK::new_window));

    /* Reverse mapping should point to 'b' (most recent) */
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'b'));

    /* Chain an action to 'b', which should restore 'a' in the reverse map */
    ASSERT_TRUE(h.s.appendChain(BAction::make(AK::new_tab)) == Set::RecurseError::none);

    /* Now reverse mapping should point to 'a' */
    ASSERT_TRUE(reverseIs(h.s, AK::new_window, 'a'));
}

/* Wisp: write the trigger then formatEntries into a fixed buffer, as
 * formatEntry in Config.zig does. */
static std::string formatEntriesOf(const Set &s, const Trigger &t) {
    const Set::Entry *entry = s.get(t);
    if (entry == nullptr) return std::string();
    std::string output;
    std::string storage;
    size_t end = 0;
    Set::Value::bufPrint(&storage, &end, entry->key.format());
    entry->value.formatEntries(&storage, &end, wisp::config::formatter::entryFormatter("keybind", &output));
    return output;
}

TEST(input_binding, set_formatEntries_leaf_chained) {
    SetHolder h;

    /* Create a chained binding */
    ASSERT_TRUE(h.s.parseAndPut("a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);

    /* Verify it's a leaf_chained */
    ASSERT_TRUE(valueOf(h.s, uni('a')).tag == Set::Value::Tag::leaf_chained);

    /* Format the entries */
    const std::string expected = "keybind = a=new_window\n"
                                 "keybind = chain=new_tab\n";
    ASSERT_TRUE(expected == formatEntriesOf(h.s, uni('a')));
}

TEST(input_binding, set_formatEntries_leaf_chained_multiple_chains) {
    SetHolder h;

    /* Create a chained binding with 3 actions */
    ASSERT_TRUE(h.s.parseAndPut("ctrl+a=new_window") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=new_tab") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=close_surface") == Error::none);

    /* Verify it's a leaf_chained with 3 actions */
    ASSERT_TRUE(valueOf(h.s, uni('a', mods(false, true))).tag == Set::Value::Tag::leaf_chained);
    ASSERT_EQ(3L, (long)chainedOf(h.s, uni('a', mods(false, true))).actions.size());

    /* Format the entries */
    const std::string expected = "keybind = ctrl+a=new_window\n"
                                 "keybind = chain=new_tab\n"
                                 "keybind = chain=close_surface\n";
    ASSERT_TRUE(expected == formatEntriesOf(h.s, uni('a', mods(false, true))));
}

TEST(input_binding, set_formatEntries_leaf_chained_with_text_action) {
    SetHolder h;

    /* Create a chained binding with text actions */
    ASSERT_TRUE(h.s.parseAndPut("a=text:hello") == Error::none);
    ASSERT_TRUE(h.s.parseAndPut("chain=text:world") == Error::none);

    /* Format the entries */
    const std::string expected = "keybind = a=text:hello\n"
                                 "keybind = chain=text:world\n";
    ASSERT_TRUE(expected == formatEntriesOf(h.s, uni('a')));
}

TEST(input_binding, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == wisp::zigstd::testing_state().live); }
