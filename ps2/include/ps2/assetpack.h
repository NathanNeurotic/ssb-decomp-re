/*
 * ps2/include/ps2/assetpack.h - on-disk format of SSB64.DAT (little-endian).
 *
 * Produced by ps2/tools/build_assets.py from the user's own ROM. The pack is
 * a list of *virtual ROM regions*: every address range the game reads through
 * the N64 PI (syDmaReadRom / osEPiStartDma) is served from a region whose
 * contents have already been converted to the PS2's native representation:
 *
 *   - relocData table + files: compiled natively from the typed decomp C
 *     sources, re-encoded in the game's own reloc-chain format with native
 *     endianness, so src/lb/lbreloc.c runs unmodified.
 *   - particle script/texture banks, audio banks, sequences: likewise
 *     compiled natively from their decompiled sources.
 *
 * Regions flagged RESIDENT (tables, extern-id lists: tiny and read in
 * 2-byte pieces) are loaded into EE RAM at boot; the rest stay on disk and
 * are read on demand.
 */
#ifndef PS2_ASSETPACK_H
#define PS2_ASSETPACK_H

#include <stdint.h>

#define PS2PACK_MAGIC "SSB64PS2"
#define PS2PACK_VERSION 1

#define PS2PACK_REGION_RESIDENT 0x1u

typedef struct PS2PackHeader
{
    char magic[8];
    uint32_t version;
    uint32_t region_count;
    uint32_t region_table_offset;
    uint32_t resident_bytes;     /* sum of RESIDENT region sizes */
    uint32_t total_size;
    uint32_t reloc_vrom;         /* virtual ROM address of the reloc table */
    uint32_t reloc_file_count;
    uint32_t resident_offset;    /* all RESIDENT regions, contiguous */
    uint32_t reserved[2];
    char build_id[16];
} PS2PackHeader; /* 64 bytes */

typedef struct PS2PackRegion
{
    uint32_t vrom_start;  /* sorted ascending, non-overlapping */
    uint32_t size;
    uint32_t file_offset;
    uint32_t flags;
} PS2PackRegion;

#endif /* PS2_ASSETPACK_H */
