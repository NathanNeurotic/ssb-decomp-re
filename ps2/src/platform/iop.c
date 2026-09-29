/*
 * IOP bring-up.
 *
 * IOP RAM is ~2 MiB and shared with the BIOS modules, so only the drivers the
 * port actually needs are loaded, and boot-device drivers only for the device
 * we were launched from:
 *
 *   always:   iomanX + fileXio      (file access through one API)
 *             sio2man + mtapman + padman   (controllers, multitap)
 *             mcman + mcserv        (memory card saves)
 *   later:    libsd + sdr           (audio, loaded once the GS is up)
 *   mass:     bdm + bdmfs_fatfs + usbd_mini + usbmass_bd_mini
 *   hdd:      ps2dev9 + ps2atad + ps2hdd + ps2fs (+ pfs0: mount)
 *   mmce:     mmceman
 *   host:     nothing - the IOP is NOT reset so ps2link/PCSX2 host: survives
 *
 * No game data ever lives on the IOP.
 */
/* fileXio is only initialised here (it then backs newlib's POSIX I/O). */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <fileXio_rpc.h>
#include <kernel.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <malloc.h>
#include <sifrpc.h>
#include <string.h>

#define DECLARE_IRX(name)                         \
    extern unsigned char name##_irx[] __attribute__((aligned(16))); \
    extern unsigned int size_##name##_irx

DECLARE_IRX(iomanx);
DECLARE_IRX(filexio);
DECLARE_IRX(sio2man);
DECLARE_IRX(mtapman);
DECLARE_IRX(padman);
DECLARE_IRX(mcman);
DECLARE_IRX(mcserv);
DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mmceman);
DECLARE_IRX(libsd);
DECLARE_IRX(sdr);

#define MAX_TRACKED_MODULES 24

static const char *sLoaded[MAX_TRACKED_MODULES];
static int sLoadedCount;
static int sIopWasReset;

typedef int (*IrxPatchFn)(uint8_t *image, unsigned int size);

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* `addiu $rt, $zero, 2` or `ori $rt, $zero, 2` */
static int is_li_2(uint32_t w)
{
    uint32_t op = w >> 26;

    return (op == 0x09 || op == 0x0D) && ((w >> 21) & 31) == 0 && ((w >> 16) & 31) != 0 && (w & 0xFFFF) == 2;
}

/* ps2sdk's sdrdrv (a reimplementation of SCE's SDRDRV 4.0.1) ends its
 * module_start with `return MODULE_REMOVABLE_END` (2).  Only IOP MODLOAD
 * versions newer than 1.2 know that value (ps2sdk loadcore.h); those come
 * with game IOPRP images or newer BIOSes.  After our IOP reset the console's
 * own rom0:MODLOAD is in charge, and an older one treats 2 as "not
 * resident": it unloads the module while its RPC thread, just started, runs
 * from that memory, and the IOP never answers the load RPC.  That is the
 * hang at "IOP: loading sdr" on hardware; PCSX2's BIOS image accepts 2.
 *
 * The fix is to make the module report MODULE_RESIDENT_END (0), which every
 * MODLOAD accepts and which is what the module means anyway (it is never
 * unloaded).  The return follows the module's " Exit rsd_main " Kprintf, so
 * find the code that loads that string's address and patch the first
 * `li v0, 2` after it.  Returns 0 when patched. */
static int patch_sdr_resident(uint8_t *img, unsigned int size)
{
    static const char kMarker[] = "Exit rsd_main";
    uint32_t phoff, phnum, phentsize, i;
    uint32_t seg_off = 0, seg_vaddr = 0, seg_size = 0;
    uint32_t str_off = 0, str_vaddr, lo, hi, w;
    int found = 0;

    if (size < 0x34 || rd32(img) != 0x464C457Fu) /* "\x7FELF" */
    {
        return -1;
    }
    phoff = rd32(img + 0x1C);
    phentsize = (uint32_t)img[0x2A] | ((uint32_t)img[0x2B] << 8);
    phnum = (uint32_t)img[0x2C] | ((uint32_t)img[0x2D] << 8);
    for (i = 0; i < phnum; i++)
    {
        const uint8_t *ph = img + phoff + i * phentsize;

        if (phoff + (i + 1) * phentsize > size)
        {
            return -1;
        }
        if (rd32(ph) == 1) /* PT_LOAD */
        {
            seg_off = rd32(ph + 4);
            seg_vaddr = rd32(ph + 8);
            seg_size = rd32(ph + 16); /* p_filesz */
            found = 1;
            break;
        }
    }
    if (!found || seg_off + seg_size > size)
    {
        return -1;
    }

    /* the marker string (back up to its start: it begins with a space) */
    for (i = seg_off; i + sizeof(kMarker) - 1 <= seg_off + seg_size; i++)
    {
        if (memcmp(img + i, kMarker, sizeof(kMarker) - 1) == 0)
        {
            str_off = i;
            while (str_off > seg_off && img[str_off - 1] != '\0')
            {
                str_off--;
            }
            break;
        }
    }
    if (str_off == 0)
    {
        return -1;
    }
    str_vaddr = str_off - seg_off + seg_vaddr;
    lo = str_vaddr & 0xFFFF;
    hi = ((str_vaddr + 0x8000) >> 16) & 0xFFFF;

    /* addiu $a0, $rs, %lo(str) preceded by lui $rs, %hi(str) */
    for (i = seg_off; i + 4 <= seg_off + seg_size; i += 4)
    {
        uint32_t j, k, rs;

        w = rd32(img + i);
        if ((w >> 26) != 0x09 || ((w >> 16) & 31) != 4 || (w & 0xFFFF) != lo)
        {
            continue;
        }
        rs = (w >> 21) & 31;
        found = 0;
        for (j = 1; j <= 8 && i >= seg_off + j * 4; j++)
        {
            uint32_t l = rd32(img + i - j * 4);

            if ((l >> 26) == 0x0F && ((l >> 16) & 31) == rs && (l & 0xFFFF) == hi)
            {
                found = 1;
                break;
            }
        }
        if (!found)
        {
            continue;
        }
        /* The Kprintf call follows, then the constant 2 is loaded into the
         * return register: $v0 directly, or - when module_start is inlined
         * into _start - a callee-saved register that the shared epilogue
         * moves to $v0 (often in the delay slot of the jump there).  Take
         * the first `addiu/ori $rt, $zero, 2` up to and including the delay
         * slot of the first `jr ra`. */
        for (k = i + 4; k < i + 4 + 24 * 4 && k + 4 <= seg_off + seg_size; k += 4)
        {
            w = rd32(img + k);
            if (is_li_2(w))
            {
                wr32(img + k, w & 0xFFFF0000u);
                return 0;
            }
            if (w == 0x03E00008u) /* jr ra: check its delay slot, then stop */
            {
                w = rd32(img + k + 4);
                if (is_li_2(w))
                {
                    wr32(img + k + 4, w & 0xFFFF0000u);
                    return 0;
                }
                break;
            }
        }
    }
    return -1;
}

static int load_irx(const char *name, void *buf, unsigned int size, const char *args, int args_len,
                    IrxPatchFn patch)
{
    int result = 0;
    int id;
    void *src = buf;
    void *bounce = NULL;

    /* SifExecModuleBuffer() sends the image with a SIF DMA REF tag, and the
     * DMAC only addresses whole quadwords.  A blob that the embedding step
     * did not place on a 16-byte boundary reaches the IOP shifted (or not
     * at all) on hardware, while PCSX2 tolerates it.  Copy such a blob to an
     * aligned buffer first; modules that need a patch are copied too. */
    if (((uintptr_t)buf & 15) != 0 || patch != NULL)
    {
        bounce = memalign(64, (size + 63) & ~63u);
        if (bounce == NULL)
        {
            ps2_log("IOP: %s: no memory for an aligned copy", name);
            return -1;
        }
        memcpy(bounce, buf, size);
        src = bounce;
        if (((uintptr_t)buf & 15) != 0)
        {
            ps2_log("IOP: %s realigned from %p", name, buf);
        }
        if (patch != NULL)
        {
            if (patch((uint8_t *)bounce, size) != 0)
            {
                ps2_log("IOP: %s: patch site not found, not loading it", name);
                free(bounce);
                return -1;
            }
            ps2_log("IOP: %s patched", name);
        }
    }

    id = SifExecModuleBuffer(src, size, (u32)args_len, args, &result);
    if (bounce != NULL)
    {
        free(bounce);
    }

    if (id < 0 || result < 0 || result == 1 /* NO_RESIDENT_END means unloaded */)
    {
        ps2_log("IOP: %s failed (id=%d res=%d)", name, id, result);
        return -1;
    }
    if (sLoadedCount < MAX_TRACKED_MODULES)
    {
        sLoaded[sLoadedCount++] = name;
    }
    ps2_log("IOP: loaded %s (%u bytes)", name, size);
    return 0;
}

/* Every module gets its own boot-stage colour, shown before the load
 * starts, so a hang inside one SifExecModuleBuffer() call names the module
 * on the TV even while the GS is not set up yet (see PS2_PORT.md,
 * "Troubleshooting on hardware"). */
#define LOAD_IRX_PATCHED(name, rgb, patch) \
    (ps2_boot_stage("IOP: loading " #name, (rgb)), load_irx(#name, name##_irx, size_##name##_irx, NULL, 0, (patch)))
#define LOAD_IRX(name, rgb) LOAD_IRX_PATCHED(name, rgb, NULL)

void ps2_iop_init(void)
{
    PS2BootDevice dev = ps2_storage_boot_device();

    SifInitRpc(0);

    /* PCSX2 normally boots this port from host:, so it never exercises the
     * IOP reboot below.  Real hardware usually boots from mass:/mc:/mmce:.
     *
     * stdout/stderr use an IOP RPC by default in ps2sdk/newlib.  Do not print
     * while the IOP is being rebooted or while its RPC servers are being
     * rebuilt: a stale console RPC can wait forever on hardware.  ps2_log()
     * still records every line in RAM while console mirroring is disabled.
     *
     * The extra colours deliberately subdivide the old magenta "IOP" stage,
     * so if hardware still stops here the visible colour identifies exactly
     * which operation did not return. */
    if (dev != PS2_BOOT_HOST)
    {
        ps2_log_console(0);
        ps2_boot_stage("IOP: reset request", 0x800080);       /* magenta */
        while (!SifIopReset("", 0))
        {
        }
        while (!SifIopSync())
        {
        }
        ps2_boot_stage("IOP: reset synced", 0x804000);        /* orange */
        SifInitRpc(0);
        sIopWasReset = 1;
    }

    ps2_boot_stage("IOP: RPC + loader", 0x808000);            /* yellow */
    SifLoadFileInit();
    SifInitIopHeap();

    /* Allow SifExecModuleBuffer on every BIOS revision. */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    if (LOAD_IRX(iomanx, 0x406000) < 0 ||   /* olive */
        LOAD_IRX(filexio, 0x408000) < 0)    /* yellow-green */
    {
        goto module_failure;
    }
    ps2_boot_stage("IOP: fileXio RPC", 0x60A000); /* lime */
    if (fileXioInit() < 0)
    {
        goto module_failure;
    }

    if (LOAD_IRX(sio2man, 0x008000) < 0 ||  /* green */
        LOAD_IRX(mtapman, 0x006020) < 0 ||  /* dark green */
        LOAD_IRX(padman, 0x404040) < 0 ||   /* grey */
        LOAD_IRX(mcman, 0x804040) < 0 ||    /* pink */
        LOAD_IRX(mcserv, 0x402000) < 0)     /* brown */
    {
        goto module_failure;
    }

    /* The sound drivers (libsd + sdrdrv) are loaded later, once the GS is up
     * and the boot log is on screen: see ps2_iop_load_audio_drivers(). */

    /* Keep printf mirroring disabled after a real-hardware IOP reboot.
     * Even with fileXio restored, stdout may still reference launcher/RPC
     * state that existed before the reset.  PCSX2's normal host: path never
     * reboots the IOP, so leave console logging enabled there only.
     *
     * ps2_log() still records every line in EE RAM, and hardware can save
     * that history to SSB64.LOG once the boot device is mounted. */
    if (!sIopWasReset)
    {
        ps2_log_console(1);
    }
    ps2_log("IOP: %s, %d modules", sIopWasReset ? "reset" : "kept (host boot)", sLoadedCount);
    return;

module_failure:
    /* Keep this path independent of printf/file I/O: those services are the
     * very thing that may have failed.  A solid red screen is therefore an
     * unambiguous base-module failure instead of another possible deadlock. */
    ps2_log_console(0);
    ps2_boot_stage("IOP: module failure", 0x800000);
    for (;;)
    {
        SleepThread();
    }
}

int ps2_iop_load_audio_drivers(void)
{
    /* Audio is optional: a failure here leaves the game silent instead of
     * stopping the boot. */
    if (LOAD_IRX(libsd, 0x008060) < 0 || LOAD_IRX_PATCHED(sdr, 0x006080, patch_sdr_resident) < 0)
    {
        ps2_log("IOP: sound drivers unavailable; audio stays silent");
        return -1;
    }
    return 0;
}

void ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_MASS:
    case PS2_BOOT_UNKNOWN: /* unrecognised launcher path: USB is the best guess */
        /* USB mass storage enumerates asynchronously; boot.c waits for the
         * asset pack to become visible. */
        LOAD_IRX(bdm, 0x200060);
        LOAD_IRX(bdmfs_fatfs, 0x400060);
        LOAD_IRX(usbd_mini, 0x600060);
        LOAD_IRX(usbmass_bd_mini, 0x800060);
        break;

    case PS2_BOOT_MMCE:
        LOAD_IRX(mmceman, 0x600060);
        break;

    case PS2_BOOT_HDD:
        /* HDD boot needs dev9/atad/hdd/pfs plus a partition mount; not yet
         * wired up (see PS2_PORT_STATUS.md). */
        ps2_log("IOP: HDD boot drivers not implemented yet");
        break;

    default:
        break;
    }
}

int ps2_iop_module_loaded(const char *name)
{
    int i;

    for (i = 0; i < sLoadedCount; i++)
    {
        if (strcmp(sLoaded[i], name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

int ps2_iop_module_count(void)
{
    return sLoadedCount;
}

const char *ps2_iop_module_name(int i)
{
    return (i >= 0 && i < sLoadedCount) ? sLoaded[i] : "";
}
