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
static char sPackPath[300];
static int sLastSeekRc;
static int sLastReadGot;
static int sStreamReopens;
static PS2PackHeader sHeader;
static PS2PackRegion *sRegions;
static uint8_t **sResidentPtr; /* per region: EE copy or NULL */
static uint8_t *sResidentBlob;
static int sReadSema = -1;

static uint32_t sBytesRead;
static uint32_t sReads;
static uint32_t sMisses;

static int reopen_pack_stream(void)
{
    int new_fd;

    if (sPackPath[0] == '\0')
        return 0;

    /*
     * Reopen through the same public filesystem path. This is intentionally
     * backend-agnostic: a removable/network/MMCE transport that loses one
     * descriptor can recover without changing drivers or rebooting the IOP.
     */
    new_fd = ps2_file_open_read(sPackPath);
    if (new_fd < 0)
        return 0;

    if (sFd >= 0)
        ps2_file_close(sFd);
    sFd = new_fd;
    sStreamReopens++;
    ps2_log("assets: reopened persistent pack stream (%d)", sStreamReopens);
    return 1;
}

static int read_exact(void *dst, uint32_t offset, uint32_t size)
{
    extern void ps2_delay_vblanks(int n);
    int attempt;

    sLastSeekRc = 0;
    sLastReadGot = 0;

    /*
     * Every attempt starts from the absolute pack offset. A failed descriptor
     * is reopened through the same device/path before the next attempt; no
     * backend swap, IOP reset, or MMCE-specific descriptor handoff occurs.
     */
    for (attempt = 0; attempt < 4; attempt++)
    {
        sLastSeekRc = ps2_file_seek(sFd, offset);
        if (sLastSeekRc < 0)
        {
            ps2_log("assets: seek failed off=0x%08x rc=%d attempt=%d",
                    (unsigned)offset, sLastSeekRc, attempt + 1);
        }
        else
        {
            sLastReadGot = ps2_file_read(sFd, dst, size);
            if (sLastReadGot == (int)size)
                return 1;

            ps2_log("assets: short/read fail off=0x%08x want=%u got=%d attempt=%d",
                    (unsigned)offset, (unsigned)size, sLastReadGot, attempt + 1);
        }

        if (attempt + 1 < 4)
        {
            ps2_delay_vblanks(2);
            reopen_pack_stream();
        }
    }

    return 0;
}

int ps2_assets_init(void)
{
    char path[300];
    uint32_t i, table_bytes;
    ee_sema_t sema = { 0 };

    sema.init_count = 1;
    sema.max_count = 1;
    sReadSema = CreateSema(&sema);
    if (sReadSema < 0)
    {
        ps2_log("assets: read semaphore creation failed (%d)", sReadSema);
        return 0;
    }

    /*
     * Wait for the real pack by opening the handle we will keep for the
     * entire run. Do not probe-open/close it in boot.c and then reopen it:
     * removable/network/MMCE transports can still be settling, and MMCE in
     * particular should not be forced through redundant file-open traffic.
     */
    {
        extern void ps2_delay_vblanks(int n);
        int attempt;

        sFd = -1;
        for (attempt = 0; attempt < 200; attempt++)
        {
            int root_ready = ps2_storage_resolve_data_root(PACK_NAME);

            /*
             * Do not touch SSB64.DAT until the BDM filesystem says the
             * intended slot is actually mounted. On hardware, calling open()
             * on massN: while USB is still enumerating can block inside the
             * filesystem and prevent this retry loop from ever advancing.
             *
             * RiptOPL solves the same boot problem by resolving the literal
             * mass slot first, then escalating transports in bounded tiers.
             */
            if (!root_ready &&
                (ps2_storage_data_device() == PS2_BOOT_BDM ||
                 ps2_storage_data_device() == PS2_BOOT_USB ||
                 ps2_storage_data_device() == PS2_BOOT_ATA ||
                 ps2_storage_data_device() == PS2_BOOT_MX4SIO ||
                 ps2_storage_data_device() == PS2_BOOT_ILINK ||
                 ps2_storage_data_device() == PS2_BOOT_UDPBD))
            {
                if (ps2_storage_data_device() == PS2_BOOT_BDM)
                {
                    /*
                     * This is the required game-data resolve, not RiptOPL's
                     * short first-run config bootstrap. Give USB the full
                     * 3-second RiptOPL resolve budget before touching MX4SIO,
                     * which shares SIO2 with PAD/MC. Hardware showed USB
                     * mounting after our old 1.5-second threshold, causing an
                     * unnecessary mx4sio_bd load on a USB boot.
                     */
                    if (attempt == 30)
                        ps2_iop_load_bdm_fallback_transports(); /* MX4SIO */
                    else if (attempt == 60)
                        ps2_iop_load_bdm_fallback_transports(); /* iLink + ATA */
                }

                if (attempt == 0 || attempt == 30 || attempt == 60 || attempt == 100)
                    ps2_log("assets: waiting for BDM slot identity (try %d)", attempt + 1);

                ps2_delay_vblanks(6);
                continue;
            }

            ps2_storage_path(path, sizeof(path), PACK_NAME);
            snprintf(sPackPath, sizeof(sPackPath), "%s", path);
            sFd = ps2_file_open_read(sPackPath);

            /* PCSX2 Run ELF can lose argv[0]'s directory separators. */
            if (sFd < 0 && ps2_storage_boot_device() == PS2_BOOT_HOST)
            {
                snprintf(path, sizeof(path), "host:%s", PACK_NAME);
                snprintf(sPackPath, sizeof(sPackPath), "%s", path);
                sFd = ps2_file_open_read(sPackPath);
            }

            if (sFd >= 0)
            {
                ps2_log("assets: persistent %s opened after %d ms", path, attempt * 100);
                break;
            }

            if (ps2_storage_data_device() == PS2_BOOT_HOST)
                break;

            ps2_delay_vblanks(6);
        }
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

    /*
     * Treat the DAT header as untrusted input before using any field for an
     * allocation, read, or pointer offset. A corrupt/partial pack should
     * produce one deterministic boot error, never an overflow or NULL write.
     */
    if (sHeader.total_size < sizeof(sHeader) ||
        sHeader.region_count == 0 ||
        sHeader.region_count > (UINT32_MAX / (uint32_t)sizeof(PS2PackRegion)) ||
        sHeader.region_count > (UINT32_MAX / (uint32_t)sizeof(uint8_t *)))
    {
        ps2_log("assets: invalid pack sizing (regions=%u total=%u)",
                (unsigned)sHeader.region_count, (unsigned)sHeader.total_size);
        return 0;
    }

    table_bytes = sHeader.region_count * (uint32_t)sizeof(PS2PackRegion);
    if (sHeader.region_table_offset > sHeader.total_size ||
        table_bytes > sHeader.total_size - sHeader.region_table_offset ||
        sHeader.resident_offset > sHeader.total_size ||
        sHeader.resident_bytes > sHeader.total_size - sHeader.resident_offset)
    {
        ps2_log("assets: pack table/resident range exceeds declared size");
        return 0;
    }

    sRegions = ps2_mem_alloc(PS2_MEM_GAME_HEAP, table_bytes, 64);
    sResidentPtr = ps2_mem_alloc(PS2_MEM_GAME_HEAP,
                                sHeader.region_count * sizeof(uint8_t *), 64);
    if (sRegions == NULL || sResidentPtr == NULL)
    {
        ps2_log("assets: cannot allocate region metadata (%u entries)",
                (unsigned)sHeader.region_count);
        return 0;
    }
    if (!read_exact(sRegions, sHeader.region_table_offset, table_bytes))
    {
        ps2_log("assets: region table read failed");
        return 0;
    }

    /* Validate every region before any pointer arithmetic below. */
    for (i = 0; i < sHeader.region_count; i++)
    {
        const PS2PackRegion *r = &sRegions[i];
        uint32_t end;

        if (r->size == 0 || r->file_offset > sHeader.total_size ||
            r->size > sHeader.total_size - r->file_offset ||
            r->vrom_start > UINT32_MAX - r->size)
        {
            ps2_log("assets: invalid region %u range", (unsigned)i);
            return 0;
        }
        end = r->vrom_start + r->size;
        if (i > 0)
        {
            const PS2PackRegion *prev = &sRegions[i - 1];
            uint32_t prev_end = prev->vrom_start + prev->size;

            if (r->vrom_start < prev_end)
            {
                ps2_log("assets: region table is unsorted/overlapping at %u", (unsigned)i);
                return 0;
            }
        }
        (void)end;

        if (r->flags & PS2PACK_REGION_RESIDENT)
        {
            if (r->file_offset < sHeader.resident_offset ||
                r->file_offset - sHeader.resident_offset > sHeader.resident_bytes ||
                r->size > sHeader.resident_bytes - (r->file_offset - sHeader.resident_offset))
            {
                ps2_log("assets: resident region %u escapes resident block", (unsigned)i);
                return 0;
            }
        }
    }

    /* All resident regions are stored back to back: one read at boot. */
    sResidentBlob = NULL;
    if (sHeader.resident_bytes != 0)
    {
        sResidentBlob = ps2_mem_alloc(PS2_MEM_GAME_HEAP, sHeader.resident_bytes, 64);
        if (sResidentBlob == NULL)
        {
            ps2_log("assets: cannot allocate %u-byte resident block",
                    (unsigned)sHeader.resident_bytes);
            return 0;
        }
        if (!read_exact(sResidentBlob, sHeader.resident_offset, sHeader.resident_bytes))
        {
            ps2_log("assets: resident block read failed");
            return 0;
        }
    }
    for (i = 0; i < sHeader.region_count; i++)
    {
        sResidentPtr[i] = (sRegions[i].flags & PS2PACK_REGION_RESIDENT)
                              ? sResidentBlob + (sRegions[i].file_offset - sHeader.resident_offset)
                              : NULL;
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

uint32_t ps2_rom_region_remaining(uint32_t rom_addr)
{
    int idx;

    if ((rom_addr & 0xF0000000u) == 0xB0000000u)
        rom_addr &= 0x0FFFFFFFu;

    idx = (sRegions != NULL) ? find_region(rom_addr) : -1;
    if (idx < 0)
        return 0;

    return sRegions[idx].size - (rom_addr - sRegions[idx].vrom_start);
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
                ps2_panic("asset read failed vrom=0x%08x file=0x%08x size=%u got=%d seek=%d reopens=%d",
                          (unsigned)rom_addr, (unsigned)(r->file_offset + off), (unsigned)n,
                          sLastReadGot, sLastSeekRc, sStreamReopens);
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
