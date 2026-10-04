/*
 * IOP bring-up for real launch/data devices.
 *
 * Normal launches keep the launcher's live IOP/filesystem because SSB64.DAT
 * is a sidecar beside SSB64.ELF. An explicit --data override is the only path
 * that rebuilds a separate data-device stack from a clean IOP.
 *
 * Base:      iomanX + fileXio + sio2man + mtapman + padman + mcman/mcserv
 *            + libsd/sdr
 * USB:       bdm + bdmfs_fatfs + usbd_mini + usbmass_bd_mini
 * ATA BDM:   ps2dev9 + bdm + bdmfs_fatfs + ps2atad
 * MX4SIO:    bdm + bdmfs_fatfs + mx4sio_bd
 * iLink:     bdm + bdmfs_fatfs + iLinkman + IEEE1394_bd
 * UDPBD:     ps2dev9 + bdm + bdmfs_fatfs + smap_udpbd(ip=...)
 * UDPFS:     ps2dev9 + udpfs_smap + udpfs_ministack(ip=...) + udpfs_ioman
 * APA/PFS:   ps2dev9 + bdm + bdmfs_fatfs + ps2atad + ps2hdd + ps2fs
 * MMCE:      mmceman
 */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <ctype.h>
#include <delaythread.h>
#include <fcntl.h>
#include <fileXio_rpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <io_common.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DECLARE_IRX(name)                 \
    extern unsigned char name##_irx[];    \
    extern unsigned int size_##name##_irx

DECLARE_IRX(iomanx);
DECLARE_IRX(filexio);
DECLARE_IRX(sio2man);
DECLARE_IRX(mtapman);
DECLARE_IRX(padman);
DECLARE_IRX(mcman);
DECLARE_IRX(mcserv);
DECLARE_IRX(libsd);
DECLARE_IRX(sdr);

DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mmceman);
DECLARE_IRX(cdvd);

DECLARE_IRX(ps2dev9);
DECLARE_IRX(ps2atad);
DECLARE_IRX(mx4sio_bd);
DECLARE_IRX(iLinkman);
DECLARE_IRX(IEEE1394_bd);
DECLARE_IRX(smap_udpbd);
DECLARE_IRX(udpfs_smap);
DECLARE_IRX(udpfs_ministack);
DECLARE_IRX(udpfs_ioman);
DECLARE_IRX(ps2hdd);
DECLARE_IRX(ps2fs);
DECLARE_IRX(secrsif);

#define MAX_TRACKED_MODULES 32

static const char *sLoaded[MAX_TRACKED_MODULES];
static int sLoadedCount;
static int sIopWasReset;

static int load_irx(const char *name, void *buf, unsigned int size, const char *args, int args_len)
{
    int result = 0;
    int id = SifExecModuleBuffer(buf, size, (u32)args_len, args, &result);

    if (id < 0 || result == 1 /* NO_RESIDENT_END */)
    {
        ps2_log("IOP: %s failed (id=%d res=%d)", name, id, result);
        return -1;
    }
    if (sLoadedCount < MAX_TRACKED_MODULES)
        sLoaded[sLoadedCount++] = name;
    ps2_log("IOP: loaded %s (%u bytes)", name, size);
    return 0;
}

#define LOAD_IRX(name) load_irx(#name, name##_irx, size_##name##_irx, NULL, 0)
#define LOAD_IRX_ARGS(name, args, len) load_irx(#name, name##_irx, size_##name##_irx, args, len)

static int module_present_any(const char *a, const char *b, const char *d)
{
    if (a != NULL && SifSearchModuleByName(a) >= 0)
        return 1;
    if (b != NULL && SifSearchModuleByName(b) >= 0)
        return 1;
    if (d != NULL && SifSearchModuleByName(d) >= 0)
        return 1;
    return 0;
}

static int load_irx_if_absent(const char *label, void *buf, unsigned int size,
                              const char *a, const char *b, const char *d)
{
    if (module_present_any(a, b, d))
    {
        ps2_log("IOP: inherited %s", label);
        return 0;
    }

    ps2_log("IOP: %s missing; loading local copy", label);
    return load_irx(label, buf, size, NULL, 0);
}

#define LOAD_IRX_IF_ABSENT(name, a, b, d) \
    load_irx_if_absent(#name, name##_irx, size_##name##_irx, a, b, d)

static int load_bdm_core(void)
{
    if (LOAD_IRX(bdm) < 0)
        return -1;
    if (LOAD_IRX(bdmfs_fatfs) < 0)
        return -1;
    return 0;
}

static int is_ipv4_token(const char *s)
{
    int octets = 0;
    int digits = 0;
    int value = 0;

    for (; *s != '\0'; s++)
    {
        if (*s >= '0' && *s <= '9')
        {
            if (++digits > 3)
                return 0;
            value = value * 10 + (*s - '0');
            if (value > 255)
                return 0;
        }
        else if (*s == '.' && digits != 0 && octets < 3)
        {
            octets++;
            digits = 0;
            value = 0;
        }
        else
        {
            return 0;
        }
    }

    return octets == 3 && digits != 0;
}

static int read_ip_arg(char *out, size_t out_size)
{
    static const char *paths[] = {
        "mc0:/SYS-CONF/IPCONFIG.DAT",
        "mc1:/SYS-CONF/IPCONFIG.DAT",
    };
    char buf[128];
    int i;

    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        int fd = open(paths[i], O_RDONLY);
        int n;
        char *p, *end;

        if (fd < 0)
            continue;
        n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';

        p = buf;
        while (*p != '\0' && isspace((unsigned char)*p))
            p++;
        end = p;
        while (*end != '\0' && !isspace((unsigned char)*end))
            end++;
        *end = '\0';

        if (*p != '\0' && strlen(p) <= 15 && is_ipv4_token(p))
        {
            snprintf(out, out_size, "ip=%s", p);
            ps2_log("IOP: network IP from %s: %s", paths[i], p);
            return 1;
        }
    }

    ps2_log("IOP: network device needs mc?:/SYS-CONF/IPCONFIG.DAT");
    return 0;
}

static int mount_hdd_partition(void)
{
    const char *source = ps2_storage_hdd_mount_source();
    int attempt;
    int r = -1;

    if (source == NULL || source[0] == '\0')
    {
        ps2_log("IOP: bare pfs: path has no APA partition to remount");
        return -1;
    }

    for (attempt = 0; attempt < 40; attempt++)
    {
        r = fileXioMount("pfs0:", source, FIO_MT_RDWR);
        if (r >= 0)
        {
            ps2_log("IOP: mounted %s on pfs0:", source);
            return 0;
        }
        DelayThread(250 * 1000);
    }

    ps2_log("IOP: failed to mount %s on pfs0: (%d)", source, r);
    return -1;
}

void ps2_iop_init(void)
{
    int preserve_iop = ps2_storage_requires_iop_preserve();

    sLoadedCount = 0;
    sIopWasReset = 0;

    SifInitRpc(0);

    if (preserve_iop)
    {
        /* Sidecar mode: the filesystem that loaded this ELF is the filesystem
         * that owns SSB64.DAT. Keep it alive. Do not reboot the IOP and do not
         * reload storage drivers. Only fill in runtime services that are
         * genuinely absent, using their actual IRX module IDs to avoid the
         * duplicate-module hangs seen on hardware. */
        SifLoadFileInit();
        SifInitIopHeap();
        sbv_patch_enable_lmb();
        sbv_patch_disable_prefix_check();

        /* Never replace or add filesystem modules in sidecar mode. If the
         * launcher used fileXio, bind the EE client to that exact live server;
         * otherwise leave the inherited filesystem namespace untouched and
         * let the subsequent sidecar open report whether it is reachable. */
        if (module_present_any("IOX/File_Manager_Rpc", NULL, NULL))
            fileXioInit();
        else
            ps2_log("IOP: inherited fileXio RPC not present");

        /* Do not touch controller/card/audio services yet. First prove that
         * the inherited sidecar filesystem can actually open SSB64.DAT, then
         * add only any missing runtime services with the GS log visible. */
        ps2_log("IOP: inherited sidecar filesystem kept");
        return;
    }

    /* Explicit cross-device --data mode: rebuild from a known IOP state. */
    fileXioExit();
    SifExitRpc();
    SifInitRpc(0);

    while (!SifIopReset("", 0))
    {
    }
    while (!SifIopSync())
    {
    }
    SifInitRpc(0);
    sIopWasReset = 1;

    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    LOAD_IRX(iomanx);
    LOAD_IRX(filexio);
    fileXioInit();

    LOAD_IRX(sio2man);
    LOAD_IRX(mtapman);
    LOAD_IRX(padman);
    LOAD_IRX(mcman);
    LOAD_IRX(mcserv);
    LOAD_IRX(libsd);
    LOAD_IRX(sdr);

    ps2_log("IOP: reset/rebuilt, %d base modules", sLoadedCount);
}


int ps2_iop_prepare_runtime_services(void)
{
    int failed = 0;

    if (!ps2_storage_requires_iop_preserve())
        return 0; /* clean-reset path loaded the full base stack already */

    if (LOAD_IRX_IF_ABSENT(sio2man, "sio2man", "sio2man_logger", NULL) < 0)
        failed = 1;
    if (LOAD_IRX_IF_ABSENT(padman, "padman", NULL, NULL) < 0)
        failed = 1;

    /* Do not add multitap or memory-card modules to an inherited SIO2 stack.
     * They are optional: pad.c falls back to the native ports and save.c
     * disables persistence if the launch environment did not provide MC RPC. */

    /* Audio is independent of the storage namespace. Add its RPC only when
     * genuinely absent; spu.c still verifies sdr_driver before binding. */
    LOAD_IRX_IF_ABSENT(libsd, "freesd", "libsd", "LIBSD");
    LOAD_IRX_IF_ABSENT(sdr, "sdr_driver", NULL, NULL);

    ps2_log("IOP: inherited runtime services prepared (%d module(s) added)", sLoadedCount);
    return failed ? -1 : 0;
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    char ip_arg[24];

    /* In sidecar/inherited mode the launcher's live filesystem is already the
     * correct device stack. Loading a second mmceman/BDM/network stack is both
     * unnecessary and unsafe. */
    if (ps2_storage_requires_iop_preserve())
        return 0;

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_BDM:
    case PS2_BOOT_MC:
        return 0;

    case PS2_BOOT_CDROM:
        return LOAD_IRX(cdvd);

    case PS2_BOOT_USB:
        if (load_bdm_core() < 0 || LOAD_IRX(usbd_mini) < 0 || LOAD_IRX(usbmass_bd_mini) < 0)
            return -1;
        return 0;

    case PS2_BOOT_ATA:
        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0 || LOAD_IRX(ps2atad) < 0)
            return -1;
        return 0;

    case PS2_BOOT_MX4SIO:
        if (load_bdm_core() < 0 || LOAD_IRX(mx4sio_bd) < 0)
            return -1;
        return 0;

    case PS2_BOOT_ILINK:
        if (load_bdm_core() < 0 || LOAD_IRX(iLinkman) < 0 || LOAD_IRX(IEEE1394_bd) < 0)
            return -1;
        return 0;

    case PS2_BOOT_UDPBD:
        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0)
            return -1;
        if (!read_ip_arg(ip_arg, sizeof(ip_arg)))
            return -1;
        return LOAD_IRX_ARGS(smap_udpbd, ip_arg, (int)strlen(ip_arg) + 1);

    case PS2_BOOT_UDPFS:
        if (LOAD_IRX(ps2dev9) < 0)
            return -1;
        if (!read_ip_arg(ip_arg, sizeof(ip_arg)))
            return -1;
        if (LOAD_IRX(udpfs_smap) < 0)
            return -1;
        if (LOAD_IRX_ARGS(udpfs_ministack, ip_arg, (int)strlen(ip_arg) + 1) < 0)
            return -1;
        if (LOAD_IRX(udpfs_ioman) < 0)
            return -1;
        return 0;

    case PS2_BOOT_MMCE:
        return LOAD_IRX(mmceman);

    case PS2_BOOT_HDD:
    {
        static char hdd_args[] = "-o\0" "4\0" "-n\0" "20";
        static char pfs_args[] = "-o\0" "10\0" "-n\0" "40";

        /* A bare pfsN: data path has no APA partition name to remount.  It is
         * valid only when bootpath.c requested that the launcher's IOP/mount
         * be preserved; in that case the filesystem is already ready. */
        if (ps2_storage_hdd_mount_source()[0] == '\0')
        {
            if (ps2_storage_requires_iop_preserve())
            {
                ps2_log("IOP: using inherited PFS mount");
                return 0;
            }
            return -1;
        }

        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0 || LOAD_IRX(ps2atad) < 0)
            return -1;

        /* Proven launcHER/OSDMenu ordering: ATAD needs a short settle before
         * the APA driver probes the disk on real hardware. */
        sleep(1);

        if (LOAD_IRX_ARGS(ps2hdd, hdd_args, sizeof(hdd_args)) < 0)
            return -1;
        if (LOAD_IRX_ARGS(ps2fs, pfs_args, sizeof(pfs_args)) < 0)
            return -1;
        if (LOAD_IRX(secrsif) < 0)
            return -1;
        return mount_hdd_partition();
    }

    case PS2_BOOT_UNKNOWN:
    default:
        ps2_log("IOP: unsupported/ambiguous data device; refusing USB fallback");
        return -1;
    }
}

int ps2_iop_module_loaded(const char *name)
{
    int i;

    for (i = 0; i < sLoadedCount; i++)
    {
        if (strcmp(sLoaded[i], name) == 0)
            return 1;
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
