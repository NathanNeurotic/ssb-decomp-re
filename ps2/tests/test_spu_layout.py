#!/usr/bin/env python3
"""Source-extracted SPU sample-table bounds tests, without copyrighted ROM data.

Checks overflow, metadata offset bounds, last-sample allocation size and all
sample entry ranges. No audio timing or register behavior is modified here.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/audio/spu.c").read_text(encoding="utf-8")


def function(signature):
    at = source.index(signature)
    start = source.index("{", at)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[at:end]


functions = "\n\n".join([
    function("static int spu_sample_header_layout_valid("),
    function("static int spu_sample_last_span_valid("),
    function("static int spu_sample_entries_valid("),
])

harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    char magic[4];
    uint32_t version, count, entries_offset;
} PS2SpuSampleHeader;
typedef struct {
    uint32_t key, len, loop_start, loop_end, data_off, data_size;
    float pitch_scale;
    uint32_t flags;
} PS2SpuSampleEntry;
__FUNCTIONS__
int main(void)
{
    uint32_t words[64] = {0};
    uint8_t *set = (uint8_t *)words;
    PS2SpuSampleHeader h = {{'S','P','U','S'},1,2,16};
    PS2SpuSampleEntry entries[2] = {
      {12,20,0,0,128,64,1.0f,0},
      {24,40,0,0,192,64,1.0f,0}
    };
    uint32_t bytes=0;
    assert(sizeof(h)==16 && sizeof(entries[0])==32);
    memcpy(set+h.entries_offset, entries, sizeof(entries));
    assert(spu_sample_header_layout_valid(&h,256));
    assert(spu_sample_last_span_valid(&h,&entries[1],256,&bytes));
    assert(bytes==256);
    assert(spu_sample_entries_valid(set,bytes,&h));

    h.count=0;assert(!spu_sample_header_layout_valid(&h,256));
    h.count=65537;assert(!spu_sample_header_layout_valid(&h,256));
    h.count=2;
    h.entries_offset=12;assert(!spu_sample_header_layout_valid(&h,256));
    h.entries_offset=17;assert(!spu_sample_header_layout_valid(&h,256));
    h.entries_offset=252;assert(!spu_sample_header_layout_valid(&h,256));
    h.entries_offset=UINT32_MAX;assert(!spu_sample_header_layout_valid(&h,256));
    h.entries_offset=16;
    assert(!spu_sample_header_layout_valid(&h,15));
    assert(spu_sample_header_layout_valid(&h,256));

    entries[1].data_off=UINT32_MAX-1;
    assert(!spu_sample_last_span_valid(&h,&entries[1],256,&bytes));
    entries[1].data_off=192;
    entries[1].data_size=80;
    assert(!spu_sample_last_span_valid(&h,&entries[1],256,&bytes));
    entries[1].data_size=64;
    entries[1].data_off=79;
    assert(!spu_sample_last_span_valid(&h,&entries[1],256,&bytes));
    entries[1].data_off=192;
    assert(spu_sample_last_span_valid(&h,&entries[1],256,&bytes));

    /* The last entry is sound, but an earlier entry can still be invalid. */
    ((PS2SpuSampleEntry *)(set+16))[0].data_off=70;
    assert(!spu_sample_entries_valid(set,256,&h));
    ((PS2SpuSampleEntry *)(set+16))[0].data_off=128;
    ((PS2SpuSampleEntry *)(set+16))[0].data_size=129;
    assert(!spu_sample_entries_valid(set,256,&h));
    ((PS2SpuSampleEntry *)(set+16))[0].data_size=64;
    assert(spu_sample_entries_valid(set,256,&h));
    puts("PASS: SPU header/count, last-span overflow and all sample ranges");
    return 0;
}
""".replace("__FUNCTIONS__", functions)
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / "spu_layout.c"
    binary = Path(temp) / "spu_layout"
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    "-Wall", "-Wextra", "-Werror", str(src), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
