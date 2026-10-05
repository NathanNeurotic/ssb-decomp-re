/*
 * Asset manager: serves N64 ROM reads from SSB64.DAT.
 *
 * The game keeps addressing data by (virtual) ROM address; this module maps
 * each read onto a converted region of the pack. Only the region index and
 * the RESIDENT metadata live in EE RAM; bulk data is read straight into the
 * caller's buffer (which on the game side is the scene arena), so nothing is
 * loaded that the current scene did not ask for.
 */
#include <ps2/platform.h>
#include <ps2/assetpack.h>

#include <kernel.h>
#include <stdio.h>
#include <string.h>

#define PACK_NAME "SSB64.DAT"

static int sFd = -1;
static PS2PackHeader sHeader;
static PS2PackRegion *sRegions;
static uint8_t **sResidentPtr; /* per region: EE copy or NULL */
static uint8_t *sResidentBlob;
static int sReadSema = -1;

static uint32_t sBytesRead;
static uint32_t sReads;
static uint32_t sMisses;

static int read_exact(void *dst, uint32_t offset, uint32_t size)
{
    if (ps2_file_seek(sFd, offset) < 0)
    {
        return 0;
    }
    return ps2_file_read(sFd, dst, size) == (int)size;
}

int ps2_assets_init(void)
{
    char path[300];
    uint32_t i, table_bytes;
    ee_sema_t sema = { 0 };

    sema.init_count = 1;
    sema.max_count = 1;
    sReadSema = CreateSema(&sema);

    /* Next to the ELF first. PCSX2's "Run ELF" passes argv[0] with its
     * backslashes stripped, so for host: also try the host root, which
     * PCSX2 maps to the ELF's directory. */
    ps2_storage_path(path, sizeof(path), PACK_NAME);
    sFd = ps2_file_open_read(path);
    if (sFd < 0 && ps2_storage_boot_device() == PS2_BOOT_HOST)
    {
        ps2_log("assets: %s not found, trying host:%s", path, PACK_NAME);
        snprintf(path, sizeof(path), "host:%s", PACK_NAME);
        sFd = ps2_file_open_read(path);
    }
    if (sFd < 0)
    {
        ps2_log("assets: cannot open %s", path);
        return 0;
    }
    if (!read_exact(&sHeader, 0, sizeof(sHeader)) || memcmp(sHeader.magic, PS2PACK_MAGIC, 8) != 0 ||
        sHeader.version != PS2PACK_VERSION)
    {
        ps2_log("assets: %s is not a v%d SSB64 pack", path, PS2PACK_VERSION);
        return 0;
    }

    table_bytes = sHeader.region_count * sizeof(PS2PackRegion);
    sRegions = ps2_mem_alloc(PS2_MEM_GAME_HEAP, table_bytes, 64);
    sResidentPtr = ps2_mem_alloc(PS2_MEM_GAME_HEAP, sHeader.region_count * sizeof(uint8_t *), 64);
    if (!read_exact(sRegions, sHeader.region_table_offset, table_bytes))
    {
        ps2_log("assets: region table read failed");
        return 0;
    }

    /* All resident regions are stored back to back: one read at boot. */
    sResidentBlob = ps2_mem_alloc(PS2_MEM_GAME_HEAP, sHeader.resident_bytes + 64, 64);
    if (sHeader.resident_bytes != 0 &&
        !read_exact(sResidentBlob, sHeader.resident_offset, sHeader.resident_bytes))
    {
        ps2_log("assets: resident block read failed");
        return 0;
    }
    for (i = 0; i < sHeader.region_count; i++)
    {
        sResidentPtr[i] = (sRegions[i].flags & PS2PACK_REGION_RESIDENT)
                              ? sResidentBlob + (sRegions[i].file_offset - sHeader.resident_offset)
                              : NULL;
    }

    /* MMCEMAN is kept for setup exactly as before. Once the pack metadata and
     * resident block are proven, switch only future non-resident reads to
     * MMCEDRV, matching wOPL's in-game MMCE model. */
    if (ps2_storage_data_device() == PS2_BOOT_MMCE)
    {
        int stream_fd;

        ps2_log("assets: switching MMCE DAT to MMCEDRV runtime reads");
        stream_fd = ps2_file_mmce_enter_runtime_stream(sFd);
        if (stream_fd < 0)
            ps2_panic("MMCE runtime handoff failed: %s", ps2_file_mmce_last_error());
        sFd = stream_fd;
    }

    ps2_log("assets: %s: %u regions, %u KiB resident, %u KiB total", path, (unsigned)sHeader.region_count,
            (unsigned)(sHeader.resident_bytes >> 10), (unsigned)(sHeader.total_size >> 10));
    return 1;
}

static int find_region(uint32_t addr)
{
    int lo = 0, hi = (int)sHeader.region_count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        const PS2PackRegion *r = &sRegions[mid];

        if (addr < r->vrom_start)
            hi = mid - 1;
        else if (addr >= r->vrom_start + r->size)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

void ps2_rom_read(uint32_t rom_addr, void *dst, uint32_t size)
{
    uint8_t *out = (uint8_t *)dst;

    sReads++;
    /* PI addresses of the cartridge domain (KSEG1 0xB0000000 + offset). */
    if ((rom_addr & 0xF0000000u) == 0xB0000000u)
    {
        rom_addr &= 0x0FFFFFFFu;
    }
    while (size > 0)
    {
        int idx = (sRegions != NULL) ? find_region(rom_addr) : -1;
        const PS2PackRegion *r;
        uint32_t off, n;

        if (idx < 0)
        {
            /* Unmapped ROM: only harmless reads (boot code, unused tables)
             * should end up here. */
            sMisses++;
            if (sMisses < 32)
            {
                ps2_log("assets: unmapped ROM read 0x%08x (+%u)", (unsigned)rom_addr, (unsigned)size);
            }
            memset(out, 0, size);
            return;
        }
        r = &sRegions[idx];
        off = rom_addr - r->vrom_start;
        n = r->size - off;
        if (n > size)
        {
            n = size;
        }
        if (sResidentPtr[idx] != NULL)
        {
            memcpy(out, sResidentPtr[idx] + off, n);
        }
        else
        {
            WaitSema(sReadSema);
            if (!read_exact(out, r->file_offset + off, n))
            {
                SignalSema(sReadSema);
                ps2_panic("asset read failed at 0x%08x (%u bytes)", (unsigned)rom_addr, (unsigned)n);
            }
            SignalSema(sReadSema);
            sBytesRead += n;
        }
        out += n;
        rom_addr += n;
        size -= n;
    }
}

uint32_t ps2_assets_bytes_read(void)
{
    return sBytesRead;
}

uint32_t ps2_assets_read_count(void)
{
    return sReads;
}

uint32_t ps2_assets_reloc_vrom(void)
{
    return sHeader.reloc_vrom;
}
