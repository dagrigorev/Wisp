#!/usr/bin/env python3
"""Generate src/terminal/terminfo_xtgettcap.inc from Ghostty's terminfo entry.

Ghostty builds its XTGETTCAP response table at comptime from
src/terminfo/ghostty.zig via Source.xtgettcapMap(). Wisp has no comptime,
so this script reproduces that table exactly and emits it as a sorted C++
array of {key, response} pairs.

Usage:
    python tools/gen_terminfo.py <path to ghostty/src/terminfo/ghostty.zig> \\
        > src/terminal/terminfo_xtgettcap.inc
"""

import re
import sys


def parse_zig_string(lit):
    """Decode a Zig string literal body (the text between the quotes)."""
    out = []
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        e = lit[i]
        i += 1
        if e == "n":
            out.append("\n")
        elif e == "r":
            out.append("\r")
        elif e == "t":
            out.append("\t")
        elif e == "\\":
            out.append("\\")
        elif e == "'":
            out.append("'")
        elif e == '"':
            out.append('"')
        elif e == "x":
            out.append(chr(int(lit[i:i + 2], 16)))
            i += 2
        else:
            raise SystemExit("unhandled Zig escape: \\%s" % e)
    return "".join(out)


def split_string_literal(text, start):
    """Return (body, index after closing quote) for the literal at `start`."""
    assert text[start] == '"'
    i = start + 1
    body = []
    while True:
        c = text[i]
        if c == "\\":
            body.append(text[i:i + 2])
            i += 2
            continue
        if c == '"':
            return "".join(body), i + 1
        body.append(c)
        i += 1


def encode_string_value(v):
    """Source.xtgettcapMap's string encoding."""
    # If a string contains parameters, then we do not escape anything within
    # the string.
    if "%" in v:
        return v

    # First replace \E with the escape char (0x1B)
    result = v.replace("\\E", "\x1b")

    # Replace '^' with the control char version of that char.
    while "^" in result:
        idx = result.index("^")
        if idx > 0:
            raise SystemExit("handle control-char in middle of string: %r" % result)
        nxt = result[idx + 1]
        replacement = chr(0x7F) if nxt == "?" else chr(ord(nxt) - 64)
        # comptimeReplace replaces every occurrence of the two-char needle.
        result = result.replace(result[idx:idx + 2], replacement)
    return result


def hexencode(s):
    return "".join("%02X" % b for b in s.encode("latin-1"))


def main():
    src = open(sys.argv[1], encoding="utf-8").read()

    # names: the first entry is what TN reports. Comments in this block
    # quote terminal names, so they are stripped before the first literal
    # is located.
    names_block = src[src.index(".names = &.{"):src.index(".capabilities = &.{")]
    names_block = "\n".join(
        line.split("//")[0] for line in names_block.split("\n")
    )
    first_name_at = names_block.index('"')
    first_name = parse_zig_string(split_string_literal(names_block, first_name_at)[0])

    entries = []
    # We have all of our capabilities plus To, TN, and RGB which aren't
    # in the capabilities list but are query-able.
    entries.append(("TN", first_name))
    entries.append(("Co", "256"))
    entries.append(("RGB", "8"))

    caps = src[src.index(".capabilities = &.{"):]
    pos = 0
    pat = re.compile(r'\.\{ \.name = "')
    while True:
        m = pat.search(caps, pos)
        if m is None:
            break
        name_body, after = split_string_literal(caps, m.end() - 1)
        name = parse_zig_string(name_body)
        rest = caps[after:caps.index("}", after) + 1]
        if ".boolean" in rest:
            value = ""
        elif ".numeric" in rest:
            value = str(int(re.search(r"\.numeric = (\d+)", rest).group(1)))
        elif ".string" in rest:
            q = caps.index('"', after)
            body, after = split_string_literal(caps, q)
            value = encode_string_value(parse_zig_string(body))
        else:
            raise SystemExit("unhandled capability value: %r" % rest)
        entries.append((name, value))
        pos = after

    rows = []
    for name, value in entries:
        key = hexencode(name)
        if value == "":
            response = "\x1bP1+r%s\x1b\\" % key
        else:
            response = "\x1bP1+r%s=%s\x1b\\" % (key, hexencode(value))
        rows.append((key, response))

    seen = {}
    for key, _ in rows:
        if key in seen:
            raise SystemExit("duplicate XTGETTCAP key: %s" % key)
        seen[key] = True

    # Sorted so the lookup can binary search, which is what
    # std.StaticStringMap does with its own index.
    rows.sort(key=lambda r: r[0])

    def c_escape(s):
        out = []
        for ch in s:
            b = ord(ch)
            if ch == "\\":
                out.append("\\\\")
            elif ch == '"':
                out.append('\\"')
            elif 0x20 <= b < 0x7F:
                out.append(ch)
            else:
                out.append("\\x%02X" % b)
        # A hex escape swallows following hex digits, so break the string.
        return '"' + "".join(out).replace("\\x1B", '\\x1B" "') + '"'

    w = sys.stdout.write
    w("/* Auto-generated from Ghostty src/terminfo/ghostty.zig by\n")
    w(" * tools/gen_terminfo.py. Do not edit.\n")
    w(" *\n")
    w(" * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors\n")
    w(" * MIT License - see THIRD_PARTY_NOTICES.md\n")
    w(" *\n")
    w(" * Ghostty builds this table at comptime with Source.xtgettcapMap();\n")
    w(" * each row is a hex-encoded capability name and the full XTGETTCAP\n")
    w(" * response for it, escape sequences included. Sorted by key. */\n\n")
    w("static const XtgettcapEntry xtgettcap_entries[] = {\n")
    for key, response in rows:
        w('    {"%s", %s},\n' % (key, c_escape(response)))
    w("};\n")


if __name__ == "__main__":
    main()
