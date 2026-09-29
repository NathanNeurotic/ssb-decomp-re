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
#include <sifrpc.h>
#include <string.h>

#define DECLARE_IRX(name)            \
    extern unsigned char name##_irx[]; \
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
    int id = SifExecModuleBuffer(buf, size, (u32)args_len, args, &result);

    if (id < 0 || result == 1 /* NO_RESIDENT_END means unloaded */)
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

#define LOAD_IRX(name) load_irx(#name, name##_irx, size_##name##_irx, NULL, 0)

void ps2_iop_init(void)
{
    PS2BootDevice dev = ps2_storage_boot_device();

    SifInitRpc(0);

    /* A host: boot (ps2link / PCSX2 host fs) keeps its IOP state; every other
     * launcher leaves arbitrary modules behind, so start from a clean IOP. */
    if (dev != PS2_BOOT_HOST)
    {
        while (!SifIopReset("", 0))
        {
        }
        while (!SifIopSync())
        {
        }
        SifInitRpc(0);
        sIopWasReset = 1;
    }
    SifLoadFileInit();
    SifInitIopHeap();

    /* Allow SifExecModuleBuffer on every BIOS revision. */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    LOAD_IRX(iomanx);
    if (LOAD_IRX(filexio) == 0)
    {
        fileXioInit(); /* newlib's open/read/lseek now go through fileXio */
    }
    LOAD_IRX(sio2man);
    LOAD_IRX(mtapman);
    LOAD_IRX(padman);
    LOAD_IRX(mcman);
    LOAD_IRX(mcserv);
    LOAD_IRX(libsd);
    LOAD_IRX(sdr);

    ps2_log("IOP: %s, %d modules", sIopWasReset ? "reset" : "kept (host boot)", sLoadedCount);
}

void ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_MASS:
        LOAD_IRX(bdm);
        LOAD_IRX(bdmfs_fatfs);
        LOAD_IRX(usbd_mini);
        LOAD_IRX(usbmass_bd_mini);
        /* USB mass storage enumerates asynchronously. */
        {
            int i;

            for (i = 0; i < 60; i++) /* up to ~1 second */
            {
                extern void ps2_delay_vblanks(int n);
                ps2_delay_vblanks(1);
            }
        }
        break;

    case PS2_BOOT_MMCE:
        LOAD_IRX(mmceman);
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
