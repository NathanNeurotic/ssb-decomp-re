#!/usr/bin/env python3
"""Validate source-extracted SSB64.DAT layout guards with generated pack shapes.

The host test checks arithmetic, sorting, range and pointer safety. It does
not exercise an actual PS2 filesystem or validate a hardware DAT read.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "ps2/src/storage/assets.c").read_text(encoding="utf-8")


def extract(signature):
    at = source.index(signature)
    brace = source.index("{", at)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[at:end]


functions = "\n\n".join([
    extract("static int pack_header_layout_valid("),
    extract("static int pack_region_layout_valid("),
])

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PS2PACK_REGION_RESIDENT 1u
typedef struct {
    char magic[8];
    uint32_t version, region_count, region_table_offset, resident_bytes;
    uint32_t total_size, reloc_vrom, reloc_file_count, resident_offset;
    uint32_t reserved[2];
    char build_id[16];
} PS2PackHeader;
typedef struct {
    uint32_t vrom_start, size, file_offset, flags;
} PS2PackRegion;
__FUNCTIONS__
int main(void) {
    PS2PackHeader h = {0};
    PS2PackRegion r[3] = {
      {0x00000000u, 16, 128, PS2PACK_REGION_RESIDENT},
      {0x00001000u, 16, 144, PS2PACK_REGION_RESIDENT},
      {0x00002000u, 32, 192, 0}
    };
    h.region_count = 3;
    h.region_table_offset = 64;
    h.resident_offset = 128;
    h.resident_bytes = 32;
    h.total_size = 256;
    assert(sizeof(h) == 64 && sizeof(r[0]) == 16);
    assert(pack_header_layout_valid(&h));
    assert(pack_region_layout_valid(&h,r));

    /* The declared total must contain the table and resident segment. */
    h.region_count = 0; assert(!pack_header_layout_valid(&h)); h.region_count = 3;
    h.region_table_offset = 63; assert(!pack_header_layout_valid(&h));
    h.region_table_offset = 64;
    h.region_count = UINT32_MAX; assert(!pack_header_layout_valid(&h));
    h.region_count = 3;
    h.region_table_offset = 240; assert(!pack_header_layout_valid(&h));
    h.region_table_offset = 64;
    h.resident_offset = 111; assert(!pack_header_layout_valid(&h));
    h.resident_offset = 128;
    h.resident_bytes = 129; assert(!pack_header_layout_valid(&h));
    h.resident_bytes = 32;
    h.total_size = 0x80000000u; assert(!pack_header_layout_valid(&h));
    h.total_size = 256;
    assert(pack_header_layout_valid(&h));

    r[1].vrom_start = 8; assert(!pack_region_layout_valid(&h,r));
    r[1].vrom_start = 0x1000;
    r[2].vrom_start = 0x800; assert(!pack_region_layout_valid(&h,r));
    r[2].vrom_start = 0x2000;
    r[2].vrom_start = UINT32_MAX-1; assert(!pack_region_layout_valid(&h,r));
    r[2].vrom_start = 0x2000;
    r[2].file_offset = 250; assert(!pack_region_layout_valid(&h,r));
    r[2].file_offset = 192;
    r[2].flags = 2; assert(!pack_region_layout_valid(&h,r));
    r[2].flags = 0;
    r[0].file_offset = 127; assert(!pack_region_layout_valid(&h,r));
    r[0].file_offset = 128;
    r[1].file_offset = 150; assert(!pack_region_layout_valid(&h,r));
    r[1].file_offset = 144;
    /* Empty declared ranges are not intrinsically invalid. */
    r[2].size = 0; assert(pack_region_layout_valid(&h,r));
    r[2].size = 32;
    assert(pack_region_layout_valid(&h,r));
    puts("PASS: pack layout bounds, resident ownership, ordering, overflow");
    return 0;
}
'''.replace("__FUNCTIONS__", functions)
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / "pack_layout.c"
    exe = Path(temp) / "pack_layout"
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-O2",
                    "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
