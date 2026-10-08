"""Compile renderer coordinate code on the host and check RDP edge cases."""
from pathlib import Path
import os
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = re.search(r"^static[^\n]*\b" + re.escape(name), source, re.MULTILINE).start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    renderer = ROOT / "ps2/src/renderer"
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision", help="Check renderer code from a Git revision")
    args = parser.parse_args()
    def read(name):
        if args.revision:
            return subprocess.check_output(["git", "show", f"{args.revision}:ps2/src/renderer/{name}"], cwd=ROOT, text=True)
        return (renderer / name).read_text()
    tex = read("texcache.c")
    gbi = read("gbi.c")
    mapping = function(tex, "src_coord(")
    snap = function(gbi, "snap_leading_edge(") if "static void snap_leading_edge(" in gbi else """
static void snap_leading_edge(float *p0, float p1, float *t0, float t1)
{ (void)p0; (void)p1; (void)t0; (void)t1; }
"""
    selection = gbi[gbi.index("    ti->wrap_s_repeat =", gbi.index("static void bind_texture(")):
                    gbi.index("    /* 32-bit texels", gbi.index("static void bind_texture("))]
    source = '''#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#define G_TX_CLAMP 2
'''+mapping+"\n"+snap+'''
struct Tile { int masks, maskt, cms, cmt; };
struct Info { int wrap_s_repeat, wrap_t_repeat, clamp_s_materialized, clamp_t_materialized; };
struct Key { unsigned short clamp_width, clamp_height; };
static void selection_test(int width, int height, struct Tile *t, int ew, int eh, int cw, int ch)
{
    int w = width, h = height;
    struct Info info = {0}, *ti = &info;
    struct Key key = {0};
'''+selection+'''
    assert(w == ew && h == eh);
    assert(key.clamp_width == cw && key.clamp_height == ch);
    assert(info.clamp_s_materialized == (cw != 0));
    assert(info.clamp_t_materialized == (ch != 0));
}
int main(void)
{
    int size, x, mirror;
    unsigned long checks = 0;
    for (size = 1; size <= 1024; size++)
        for (mirror = 0; mirror <= 1; mirror++)
            for (x = 0; x < 4096; x++)
            {
                int m = x % (mirror ? size * 2 : size);
                int expected = mirror && m >= size ? size * 2 - 1 - m : m;
                int actual = src_coord(x, size, mirror);
                assert(actual == expected && actual >= 0 && actual < size);
                checks++;
            }
    { int expected[] = {0,1,2,3,3,2,1,0,0,1,2,3};
      for (x = 0; x < 12; x++) assert(src_coord(x, 4, 1) == expected[x]); }
    { struct Tile t = {2, 3, G_TX_CLAMP, G_TX_CLAMP};
      selection_test(12, 20, &t, 4, 8, 12, 20);
      selection_test(4, 8, &t, 4, 8, 0, 0);
      t.cms = t.cmt = 0;
      selection_test(12, 20, &t, 4, 8, 0, 0);
      t.masks = t.maskt = 0;
      selection_test(12, 20, &t, 12, 20, 0, 0); }
    { float p = 10.25f, uv = 0.0f;
      snap_leading_edge(&p, 20.25f, &uv, 10.0f);
      assert(p == 10.0f && uv == -0.25f);
      p = 10.0f; uv = 2.0f;
      snap_leading_edge(&p, 20.0f, &uv, 12.0f);
      assert(p == 10.0f && uv == 2.0f); }
    printf("PASS: %lu coordinate comparisons, masked/clamped tile selection, rectangle coverage\\n", checks);
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "renderer.c"
        exe = Path(tmp) / "renderer.exe"
        c.write_text(source)
        subprocess.run([os.environ.get("CC", "gcc"), "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe), "-lm"], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
