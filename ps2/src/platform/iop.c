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

static int load_irx(const char *name, void *buf, unsigned int size, const char *args, int args_len)
{
    int result = 0;
    int id;
    void *src = buf;
    void *bounce = NULL;

    /* SifExecModuleBuffer() sends the image with a SIF DMA REF tag, and the
     * DMAC only addresses whole quadwords.  A blob that the embedding step
     * did not place on a 16-byte boundary reaches the IOP shifted (or not
     * at all) on hardware, while PCSX2 tolerates it.  Copy such a blob to an
     * aligned buffer first. */
    if (((uintptr_t)buf & 15) != 0)
    {
        bounce = memalign(64, (size + 63) & ~63u);
        if (bounce == NULL)
        {
            ps2_log("IOP: %s misaligned (%p) and no memory to realign it", name, buf);
            return -1;
        }
        memcpy(bounce, buf, size);
        src = bounce;
        ps2_log("IOP: %s realigned from %p", name, buf);
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
#define LOAD_IRX(name, rgb) \
    (ps2_boot_stage("IOP: loading " #name, (rgb)), load_irx(#name, name##_irx, size_##name##_irx, NULL, 0))

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
    if (LOAD_IRX(libsd, 0x008060) < 0 || LOAD_IRX(sdr, 0x006080) < 0)
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
