"""Generate Wisp's Unicode property table from the UCD files that uucode ships.

This mirrors, in Python, what Ghostty's build does in Zig:

  uucode components (src/components.zig)  -> per-codepoint properties
  Ghostty src/build/uucode_config.zig     -> width clamping rules
  Ghostty src/unicode/lut.zig Generator   -> 3-stage lookup table

Output: a C++ include with stage1/stage2/stage3 arrays, matching
Ghostty's props_table.zig.

Usage: gen_unicode.py <ucd-dir> <out.inc>
"""
import sys
import os

UCD = sys.argv[1]
OUT = sys.argv[2]

MAX_CP = 0x10FFFF
N = MAX_CP + 1

# ---------------------------------------------------------------- UCD parsing


def lines(path):
    with open(os.path.join(UCD, path), encoding='utf-8') as f:
        for line in f:
            yield line


def parse_range(s):
    s = s.strip()
    if '..' in s:
        a, b = s.split('..')
        return int(a, 16), int(b, 16) + 1
    v = int(s, 16)
    return v, v + 1


def prop_file(path, value_map, default, missing_values=()):
    """Parse a `cp_range ; value` UCD file into a list of value_map values."""
    out = [default] * N
    for cp_range, value in missing_values:
        a, b = parse_range(cp_range)
        for cp in range(a, b):
            out[cp] = value
    for line in lines(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(';')]
        a, b = parse_range(parts[0])
        v = value_map(parts[1:])
        if v is None:
            continue
        for cp in range(a, b):
            out[cp] = v
    return out


# general_category: UnicodeData.txt, with <..., First>/<..., Last> ranges and
# Cn for every gap.
def general_category():
    gc = ['Cn'] * N
    prev = None
    range_start = None
    for line in lines('UnicodeData.txt'):
        line = line.strip()
        if not line:
            continue
        f = line.split(';')
        cp = int(f[0], 16)
        name, cat = f[1], f[2]
        if name.endswith('First>'):
            range_start = (cp, cat)
            continue
        if name.endswith('Last>'):
            start, scat = range_start
            for c in range(start, cp + 1):
                gc[c] = scat
            range_start = None
            continue
        gc[cp] = cat
        prev = cp
    return gc


# east_asian_width: default neutral, "# @missing:" lines give Wide ranges.
def east_asian_width():
    out = ['N'] * N
    for line in lines('extracted/DerivedEastAsianWidth.txt'):
        stripped = line.strip()
        if stripped.startswith('# @missing:'):
            body = stripped[len('# @missing:'):]
            parts = [p.strip() for p in body.split(';')]
            if parts[1] == 'Neutral':
                continue
            assert parts[1] == 'Wide', parts
            a, b = parse_range(parts[0])
            for cp in range(a, b):
                out[cp] = 'W'
            continue
        data = stripped.split('#')[0].strip()
        if not data:
            continue
        parts = [p.strip() for p in data.split(';')]
        a, b = parse_range(parts[0])
        for cp in range(a, b):
            out[cp] = parts[1]
    return out


def derived_core_properties():
    """Default_Ignorable_Code_Point and InCB (indic conjunct break)."""
    ignorable = [False] * N
    incb = ['none'] * N
    for line in lines('DerivedCoreProperties.txt'):
        data = line.split('#')[0].strip()
        if not data:
            continue
        parts = [p.strip() for p in data.split(';')]
        a, b = parse_range(parts[0])
        prop = parts[1]
        if prop == 'Default_Ignorable_Code_Point':
            for cp in range(a, b):
                ignorable[cp] = True
        elif prop == 'InCB':
            v = {'Linker': 'linker', 'Consonant': 'consonant', 'Extend': 'extend'}[parts[2]]
            for cp in range(a, b):
                incb[cp] = v
    return ignorable, incb


def emoji_data():
    props = {k: [False] * N for k in
             ('Emoji', 'Emoji_Presentation', 'Emoji_Modifier', 'Emoji_Modifier_Base',
              'Emoji_Component', 'Extended_Pictographic')}
    for line in lines('emoji/emoji-data.txt'):
        data = line.split('#')[0].strip()
        if not data:
            continue
        parts = [p.strip() for p in data.split(';')]
        a, b = parse_range(parts[0])
        arr = props.get(parts[1])
        if arr is None:
            continue
        for cp in range(a, b):
            arr[cp] = True
    return props


def emoji_vs_base():
    out = [False] * N
    for line in lines('emoji/emoji-variation-sequences.txt'):
        data = line.split('#')[0].strip()
        if not data:
            continue
        parts = data.split(';')[0].split()
        cp, vs = int(parts[0], 16), int(parts[1], 16)
        if vs == 0xFE0E:
            out[cp] = True
        else:
            assert vs == 0xFE0F and out[cp]
    return out


def original_grapheme_break():
    m = {'Prepend': 'prepend', 'CR': 'cr', 'LF': 'lf', 'Control': 'control',
         'Extend': 'extend', 'Regional_Indicator': 'regional_indicator',
         'SpacingMark': 'spacing_mark', 'L': 'l', 'V': 'v', 'T': 't',
         'LV': 'lv', 'LVT': 'lvt', 'ZWJ': 'zwj'}
    out = ['other'] * N
    for line in lines('auxiliary/GraphemeBreakProperty.txt'):
        data = line.split('#')[0].strip()
        if not data:
            continue
        parts = [p.strip() for p in data.split(';')]
        a, b = parse_range(parts[0])
        v = m.get(parts[1], 'other')
        for cp in range(a, b):
            out[cp] = v
    return out


# ------------------------------------------------------- uucode derived props

ZWJ = 0x200D
ZWNJ = 0x200C

# types.GraphemeBreakNoControl, in declaration order (the enum values the
# generated table stores).
GB_NO_CONTROL = [
    'other', 'prepend', 'regional_indicator', 'spacing_mark', 'l', 'v', 't',
    'lv', 'lvt', 'zwj', 'zwnj', 'extended_pictographic', 'emoji_modifier_base',
    'emoji_modifier', 'indic_conjunct_break_extend',
    'indic_conjunct_break_linker_extend', 'indic_conjunct_break_linker_other',
    'indic_conjunct_break_consonant',
]


def grapheme_break(cp, ogb, incb, emoji):
    """uucode components.zig GraphemeBreakDerived."""
    if emoji['Emoji_Modifier'][cp]:
        return 'emoji_modifier'
    if emoji['Emoji_Modifier_Base'][cp]:
        return 'emoji_modifier_base'
    if emoji['Extended_Pictographic'][cp]:
        return 'extended_pictographic'
    v = incb[cp]
    o = ogb[cp]
    if v == 'none':
        if o == 'extend':
            assert cp == ZWNJ, hex(cp)
            return 'zwnj'
        return o
    if v == 'extend':
        if cp == ZWJ:
            assert o == 'zwj'
            return 'zwj'
        assert o == 'extend'
        return 'indic_conjunct_break_extend'
    if v == 'linker':
        if o == 'extend':
            return 'indic_conjunct_break_linker_extend'
        assert o == 'other'
        return 'indic_conjunct_break_linker_other'
    assert v == 'consonant' and o == 'other'
    return 'indic_conjunct_break_consonant'


def main():
    gc = general_category()
    eaw = east_asian_width()
    ignorable, incb = derived_core_properties()
    emoji = emoji_data()
    vs_base = emoji_vs_base()
    ogb = original_grapheme_break()

    width = [0] * N                 # uucode wcwidth_standalone
    zero_in_grapheme = [False] * N  # uucode wcwidth_zero_in_grapheme
    gb = [0] * N                    # uucode grapheme_break_no_control

    for cp in range(N):
        g = grapheme_break(cp, ogb, incb, emoji)
        gb[cp] = GB_NO_CONTROL.index('other' if g in ('control', 'cr', 'lf') else g)

        # uucode components.zig Wcwidth
        c = gc[cp]
        if c in ('Cc', 'Cs', 'Zl', 'Zp'):
            w = 0
        elif cp == 0x00AD:
            w = 1
        elif ignorable[cp]:
            w = 0
        elif cp == 0x2E3A:
            w = 2
        elif cp == 0x2E3B:
            w = 3
        elif eaw[cp] in ('W', 'F'):
            w = 2
        elif g == 'regional_indicator':
            w = 2
        else:
            w = 1

        width[cp] = 2 if cp == 0x20E3 else w
        zero_in_grapheme[cp] = bool(
            w == 0 or emoji['Emoji_Modifier'][cp] or c in ('Mn', 'Me') or
            g in ('v', 't', 'prepend'))

    # Ghostty src/build/uucode_config.zig WidthComponent: the width Ghostty
    # stores, clamped to [0, 2].
    props = [0] * N
    for cp in range(N):
        if zero_in_grapheme[cp] and not emoji['Emoji_Modifier'][cp] and gb[cp] != GB_NO_CONTROL.index('prepend'):
            w = 0
        else:
            w = min(2, width[cp])
        # Properties: width u2, width_zero_in_grapheme bool, grapheme_break u5,
        # emoji_vs_base bool (Ghostty src/unicode/props.zig).
        props[cp] = (w |
                     (int(zero_in_grapheme[cp]) << 2) |
                     (gb[cp] << 3) |
                     (int(vs_base[cp]) << 8))

    # ------------------------------------------------ lut.zig Generator
    block_size = 256
    stage1, stage2, stage3 = [], [], []
    elem_index = {}
    blocks_map = {}
    block = []
    for cp in range(N):
        e = props[cp]
        idx = elem_index.get(e)
        if idx is None:
            idx = len(stage3)
            elem_index[e] = idx
            stage3.append(e)
        block.append(idx)
        if len(block) < block_size and cp != MAX_CP:
            continue
        key = tuple(block) + (0,) * (block_size - len(block))
        found = blocks_map.get(key)
        if found is None:
            found = len(stage2)
            blocks_map[key] = found
            stage2.extend(block)
        stage1.append(found)
        block = []

    assert max(len(stage1), len(stage2), len(stage3)) <= 0xFFFF

    def arr(name, values, ty):
        out = ['static const %s %s[%d] = {' % (ty, name, len(values))]
        row = []
        for v in values:
            row.append(str(v))
            if len(row) == 16:
                out.append('    ' + ','.join(row) + ',')
                row = []
        if row:
            out.append('    ' + ','.join(row) + ',')
        out.append('};')
        return '\n'.join(out)

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('/* This file is auto-generated. Do not edit.\n'
                ' *\n'
                ' * Generated from the Unicode Character Database shipped with uucode\n'
                ' * (github.com/jacobsandlund/uucode, the commit Ghostty pins), following\n'
                ' * the same derivation Ghostty uses: uucode\'s property components, the\n'
                ' * width rules in Ghostty src/build/uucode_config.zig, and the 3-stage\n'
                ' * table layout of Ghostty src/unicode/lut.zig.\n'
                ' */\n\n')
        f.write(arr('props_stage1', stage1, 'uint16_t') + '\n\n')
        f.write(arr('props_stage2', stage2, 'uint16_t') + '\n\n')
        f.write(arr('props_stage3', stage3, 'uint16_t') + '\n')

    sys.stderr.write('stage1=%d stage2=%d stage3=%d\n' % (len(stage1), len(stage2), len(stage3)))


main()
