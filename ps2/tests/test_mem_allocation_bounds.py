#!/usr/bin/env python3
"""Reject EE DMA allocation/accounting arithmetic overflow using actual source.

This tests allocation size rounding and accounting under a host mock.
It does not estimate EE fragmentation or substitute for PS2 memory profiling.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/memory/memstat.c").read_text(encoding="utf-8")

def function(signature):
    a = source.index(signature)
    b = source.index("{", a)
    depth, end = 1, b + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[a:end]

body = "\n\n".join([
    function("static uint32_t ps2_mem_checked_aligned_size("),
    function("void *ps2_mem_alloc("),
    function("void ps2_mem_free("),
])

harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
typedef int PS2MemCategory;
enum { PS2_MEM_AUDIO };
#define PS2_EE_PEAK_BUDGET 28311552u
static struct { uint32_t total_used; } sStats;
static jmp_buf checkpoint;
static int panicked, allocations, frees;
static uint32_t requested_size, requested_align;
static unsigned char buffer[256];
static void *fake_memalign(uint32_t align, uint32_t size) {
    allocations++;
    requested_align = align;
    requested_size = size;
    return size <= sizeof(buffer) ? buffer : NULL;
}
static void fake_free(void *ptr) { assert(ptr == buffer); frees++; }
static const char *ps2_mem_category_name(int cat) {(void)cat;return "audio";}
static void ps2_mem_add(int cat, int32_t delta) {
    (void)cat;
    if(delta >= 0)sStats.total_used += (uint32_t)delta;
    else sStats.total_used -= (uint32_t)-delta;
}
#define ps2_log(...) ((void)0)
#define ps2_panic(...) do {panicked++;longjmp(checkpoint,1);} while(0)
#define memalign fake_memalign
#define free fake_free
__FUNCTIONS__
#undef memalign
#undef free
int main(void) {
    assert(ps2_mem_checked_aligned_size(0) == 0);
    assert(ps2_mem_checked_aligned_size(1) == 64);
    assert(ps2_mem_checked_aligned_size(64) == 64);
    assert(ps2_mem_checked_aligned_size(65) == 128);
    assert(ps2_mem_checked_aligned_size(0x7fffffc0u) == 0x7fffffc0u);
    void *p = ps2_mem_alloc(PS2_MEM_AUDIO, 65, 8);
    assert(p == buffer && requested_size == 128 && requested_align == 16);
    assert(sStats.total_used == 128);
    ps2_mem_free(PS2_MEM_AUDIO, p, 65);
    assert(sStats.total_used == 0 && frees == 1);
    const uint32_t bad[] = {0x7fffffc1u, 0x80000000u, UINT32_MAX-62u, UINT32_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        int before = allocations;
        if(setjmp(checkpoint) == 0) {
            ps2_mem_alloc(PS2_MEM_AUDIO, bad[i], 64);
            assert(!"oversized allocation accepted");
        }
        assert(allocations == before && sStats.total_used == 0);
        assert(panicked == (int)i+1);
    }
    if(setjmp(checkpoint) == 0) {
        ps2_mem_free(PS2_MEM_AUDIO, buffer, UINT32_MAX);
        assert(!"overflowing free size accepted");
    }
    assert(frees == 1 && panicked == 5 && sStats.total_used == 0);
    puts("PASS: 64-byte EE allocation rounding, signed accounting, overflow rejection");
    return 0;
}
""".replace("__FUNCTIONS__", body)
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / "test.c"
    executable = Path(temp) / "test"
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    "-Wall", "-Wextra", "-Werror", str(src), "-o", str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
