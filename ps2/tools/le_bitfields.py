#!/usr/bin/env python3
"""Give integer-encoded script types a little-endian bitfield layout on PS2.

Several game data formats are authored as integer words built with
MSB-first shift macros (GC_FIELDSET, _FT_ANIM_CMD, aobjEvent32*, ...) and then
decoded through C bitfield structs. IDO (big-endian MIPS) allocates bitfields
from the most significant bit, GCC on the little-endian EE from the least
significant bit, so the same struct reads different bits.

For each listed type this tool adds, under `#ifdef PLATFORM_PS2`, a member
list in which every storage unit's bitfields are declared in reverse order
(with the unused bits as a leading anonymous field). Each field then covers
exactly the same bits of the data word as on the N64. The N64 declaration
stays untouched in the #else branch, so the matching build is unaffected.

Only types whose instances come from integer words are listed; types that are
filled through C initializers or at runtime are consistent under GCC as is.

Usage (repo root): python3 ps2/tools/le_bitfields.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (file, regex matching the opening of the aggregate whose *own* body holds
#  the bitfields). For anonymous structs inside a union, the regex matches the
#  inner "struct" line that follows the union's scalar members.
TARGETS = [
    ("src/sys/objtypes.h", r"union AObjEvent16\s*\{\s*s16 s;\s*u16 u;\s*struct\s*\{"),
    ("src/sys/objtypes.h", r"union AObjEvent32\s*\{\s*f32 f;\s*s32 s;\s*u32 u;\s*void \*p;\s*struct\s*\{"),
    ("src/ft/fttypes.h", r"struct FTMotionEvent\w+\s*(//[^\n]*)?\s*\{"),
    ("src/ft/fttypes.h", r"union FTKeyEvent\s*\{\s*u16 halfword;\s*struct\s*\{"),
    ("src/ft/fttypes.h", r"struct\s*\{(?=\s*ub32 is_use_xrotn_joint)"),
    ("src/gm/gmscript.h", r"struct GMColEvent\w+\s*\{"),
    ("src/gm/gmscript.h", r"struct GMRumbleEvent\w+\s*\{"),
    ("src/gm/gmtypes.h", r"struct GMColEvent\w+\s*\{"),
    ("src/gm/gmtypes.h", r"struct GMRumbleEvent\w+\s*\{"),
]

SIZES = {"u8": 8, "s8": 8, "ub8": 8, "sb8": 8,
         "u16": 16, "s16": 16, "ub16": 16, "sb16": 16,
         "u32": 32, "s32": 32, "ub32": 32, "sb32": 32, "uintptr_t": 32}

FIELD_RE = re.compile(r"^(\s*)(\w+)\s+(\w+)?\s*:\s*(\d+)\s*;(.*)$")


def convert_body(body_lines):
    """Return the little-endian member list for a struct body."""
    out = []
    unit = []  # (indent, type, name, width, comment)
    unit_type = None
    used = 0

    def flush():
        nonlocal unit, used, unit_type
        if not unit:
            return
        size = SIZES[unit_type]
        indent = unit[0][0]
        if used < size:
            out.append("%s%s : %d; // PS2: unused bits of this word" % (indent, unit_type, size - used))
        for ind, typ, name, width, comment in reversed(unit):
            out.append("%s%s %s : %d;%s" % (ind, typ, name, width, comment))
        unit, used, unit_type = [], 0, None

    for line in body_lines:
        m = FIELD_RE.match(line)
        if m and m.group(3) and m.group(2) in SIZES:
            ind, typ, name, width, comment = m.group(1), m.group(2), m.group(3), int(m.group(4)), m.group(5)
            size = SIZES[typ]
            if unit and (SIZES[unit_type] != size or used + width > size):
                flush()
            if not unit:
                unit_type = typ
            unit.append((ind, typ, name, width, comment))
            used += width
        else:
            if line.strip() == "" or line.strip().startswith("//"):
                out.append(line) if not unit else None
                continue
            flush()
            out.append(line)
    flush()
    return out


def process(path, pattern):
    full = os.path.join(ROOT, path)
    with open(full, encoding="utf-8", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    count = 0
    pos = 0
    rx = re.compile(pattern)
    while True:
        m = rx.search(text, pos)
        if not m:
            break
        body_start = m.end()  # just after "{"
        depth, i = 1, body_start
        while depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body_end = i - 1  # index of the closing "}"
        body = text[body_start:body_end]
        tail_ws = body[len(body.rstrip()):]
        body = body.rstrip()
        if "PLATFORM_PS2" in body or not re.search(r":\s*\d+\s*;", body):
            pos = body_end
            continue
        lines = body.strip("\n").split("\n")
        le = convert_body(lines)
        new_body = ("\n#ifdef PLATFORM_PS2 // little-endian bitfield order, see ps2/tools/le_bitfields.py\n" +
                    "\n".join(le) + "\n#else\n" + "\n".join(lines) + "\n#endif\n")
        # keep the indentation of the closing brace
        text = text[:body_start] + new_body + text[body_end:].lstrip(" \t") if False else \
            text[:body_start] + new_body + text[body_end:]
        pos = body_start + len(new_body)
        count += 1
    if count:
        if crlf:
            text = text.replace("\n", "\r\n")
        with open(full, "w", encoding="utf-8", newline="") as f:
            f.write(text)
    return count


def main():
    total = 0
    for path, pattern in TARGETS:
        n = process(path, pattern)
        print("%-24s %3d type(s)  %s" % (path, n, pattern[:48]))
        total += n
    print("converted %d types" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
