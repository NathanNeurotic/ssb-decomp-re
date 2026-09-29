#!/usr/bin/env python3
"""One-shot fix for non-void functions that fall off their end.

IDO happened to leave the intended value in $v0 (or the value was never used);
GCC does not, and may even treat the path as unreachable. Each fix is wrapped
in the decomp's existing `#ifdef AVOID_UB` convention, so the matching N64
build (which does not define AVOID_UB) compiles exactly as before.

Found with GCC's -Wreturn-type while bringing up the PS2 build. Already
applied in this tree; kept for reference.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (file, closing-brace line, return expression)
TAIL_RETURNS = [
    ("src/ft/ftcomputer.c", 4510, "FALSE"),
    ("src/it/itmonster/itlizardon.c", 155, "FALSE"),
    ("src/lb/lbcommon.c", 2881, "sobj"),
    ("src/libultra/n_audio/n_env.c", 5296, "0"),
    ("src/libultra/n_audio/n_env.c", 5234, "0"),
    ("src/libultra/n_audio/n_env.c", 737, "0"),
    ("src/mn/mncommon/mntitle.c", 337, "0"),
    ("src/mn/mnmaps/mnmaps.c", 505, "0"),
    ("src/mn/mnplayers/mnplayers1ptraining.c", 1039, "0"),
    ("src/mn/mnplayers/mnplayers1ptraining.c", 997, "FALSE"),
    ("src/mn/mnplayers/mnplayersvs.c", 2919, "0"),
    ("src/mn/mnplayers/mnplayersvs.c", 1550, "0"),
    ("src/mn/mnplayers/mnplayersvs.c", 1537, "0"),
    ("src/mn/mnvsmode/mnvsmode.c", 1235, "0"),
    ("src/mn/mnvsmode/mnvsmode.c", 1198, "0"),
    ("src/mn/mnvsmode/mnvsresults.c", 574, "win_player"),
    ("src/sc/sc1pmode/sc1ptrainingmode.c", 1537, "0"),
]

# Calls whose value is the function's result but is not returned.
RETURN_PREFIX = [
    ("src/it/itmain.c", "            itMainSearchRandomWeight(random, weights, min, avg);"),
    ("src/it/itmain.c", "        else itMainSearchRandomWeight(random, weights, avg, max);"),
    ("src/mn/mnplayers/mnplayers1pbonus.c",
     "\tftParamGetCostumeCommonID(fkind, ftParamGetCostumeCommonID(fkind, select_button));"),
]


def main():
    by_file = {}
    for f, line, expr in TAIL_RETURNS:
        by_file.setdefault(f, []).append((line, expr))
    for f, fixes in by_file.items():
        path = os.path.join(ROOT, f)
        with open(path, encoding="utf-8", newline="") as fh:
            lines = fh.read().split("\n")
        eol = "\r" if lines and lines[0].endswith("\r") else ""
        for line, expr in sorted(fixes, reverse=True):
            closing = lines[line - 1].rstrip("\r")
            if closing != "}":
                sys.exit("%s:%d: expected a closing brace, found %r" % (f, line, closing))
            if "#ifdef AVOID_UB" in lines[line - 4]:
                continue
            lines[line - 1:line - 1] = ["#ifdef AVOID_UB" + eol, "    return %s;%s" % (expr, eol), "#endif" + eol]
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines))
        print("fixed", f)

    for f, stmt in RETURN_PREFIX:
        path = os.path.join(ROOT, f)
        with open(path, encoding="utf-8", newline="") as fh:
            s = fh.read()
        eol = "\r\n" if "\r\n" in s else "\n"
        indent = stmt[:len(stmt) - len(stmt.lstrip())]
        body = stmt.lstrip()
        if body.startswith("else "):
            indent += "else "
            body = body[5:]
            new = "%s%s#ifdef AVOID_UB%s%sreturn%s#endif%s%s%s" % (
                stmt[:len(stmt) - len(stmt.lstrip())], "else", eol, "", eol, eol, indent.replace("else ", "    "), body)
            new = (stmt[:len(stmt) - len(stmt.lstrip())] + "else" + eol + "#ifdef AVOID_UB" + eol +
                   indent.replace("else ", "    ") + "return" + eol + "#endif" + eol +
                   indent.replace("else ", "    ") + body)
        else:
            new = "#ifdef AVOID_UB" + eol + indent + "return" + eol + "#endif" + eol + indent + body
        if new in s:
            continue
        if s.count(stmt) != 1:
            sys.exit("%s: statement not found exactly once: %r" % (f, stmt))
        s = s.replace(stmt, new)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(s)
        print("fixed", f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
