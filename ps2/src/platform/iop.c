/*
 * IOP bring-up for real launch/data devices.
 *
 * The launch device and the data device are intentionally separate.  host:
 * keeps the ps2link/PCSX2 IOP alive; every other launch starts from a clean
 * IOP and reconstructs only the stack required by the selected data device.
 * Exactly one reset happens per boot for BDM devices: once the USB host
 * driver is running, a second IOP reset leaves the stick unreachable.
 *
 * Base:      iomanX + fileXio + sio2man + mcman + mtapman + padman
 *            + libsd; dedicated ssb_audio is deferred until after storage/input
 * USB:       bdm + bdmfs_fatfs + usbd_mini + usbmass_bd_mini
 * massN:     USB stack first; MX4SIO/iLink/ATA added later only if the
 *            pack has not appeared (no reset in between)
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
DECLARE_IRX(libsd);
DECLARE_IRX(ssb_audio);

DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mmceman);
DECLARE_IRX(mmcedrv);
DECLARE_IRX(ssb_mmce_stream);
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

    /* host: and bare pfsN: data paths depend on services/mounts owned by the
     * launcher.  Everything else, including generic massN:, is rebuilt from
     * a known IOP state. Disable stdout mirroring before that reset so a
     * stale console RPC cannot deadlock real hardware. */
    if (!preserve_iop)
    {
        ps2_log_console(0);
        ps2_boot_stage("IOP: reset request", 0xC0C000);
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

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    if (sIopWasReset)
        sbv_patch_fileio();

    LOAD_IRX(iomanx);
    LOAD_IRX(filexio);

    /*
     * Match the safe SIO2 dependency order: MCMAN owns the memory-card
     * filesystem before pad/multitap clients start using SIO2. SSB64 saves
     * through MCMAN's mc0: ioman driver directly, so MCSERV/libmc are not
     * loaded at all.
     */
    LOAD_IRX(sio2man);
    LOAD_IRX(mcman);
    LOAD_IRX(mtapman);
    LOAD_IRX(padman);

    /*
     * Match RiptOPL's proven reset ordering: the USB HOST driver is resident
     * before the EE fileXio RPC client binds. The post-reset BDM/FAT and mass
     * driver are still loaded later by ps2_iop_load_boot_device_drivers().
     *
     * Do this only for USB-capable data paths. Other backends keep their
     * existing module set and ordering.
     */
    if (ps2_storage_data_device() == PS2_BOOT_BDM ||
        ps2_storage_data_device() == PS2_BOOT_USB)
    {
        if (LOAD_IRX(usbd_mini) < 0)
            ps2_log("IOP: RiptOPL USB host failed during base init");
    }

    /* Bind only after the host-side USB module ordering above, matching
     * RiptOPL's sysReset() sequence on hardware. */
    fileXioInit();

    LOAD_IRX(libsd);
    /*
     * Real hardware has already proven the embedded sdr server can wedge this
     * port during startup. Storage work must not be masked by an unrelated
     * audio RPC hang, so keep SDR deferred until the common storage path is
     * stable on hardware.
     */
    ps2_log("IOP: sdr deferred for hardware-safe storage validation");

    ps2_log("IOP: %s, %d base modules",
            sIopWasReset ? "reset" : "kept (inherited filesystem)", sLoadedCount);
}

int ps2_iop_mmce_prepare_runtime_stream(void)
{
    if (ps2_storage_data_device() != PS2_BOOT_MMCE)
        return -1;

    /*
     * MMCEMAN is the setup/filesystem driver. MMCEDRV is explicitly the MMCE
     * project's in-game streaming driver. Preserve the card-side DAT handle
     * across one deliberate reset, then rebuild the final IOP before the EE
     * controller client is initialized.
     *
     * Loading MMCEDRV beside MMCEMAN is not safe: both install SIO2MAN hooks.
     * The reset is what discards MMCEMAN's hook without issuing FS_CLOSE to
     * the card-side descriptor.
     */
    ps2_log("IOP: MMCE setup complete; rebuilding final in-game IOP");

    fileXioExit();
    SifExitIopHeap();
    SifLoadFileExit();

    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}

    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    sLoadedCount = 0;
    sIopWasReset = 1;

    if (LOAD_IRX(iomanx) < 0 || LOAD_IRX(filexio) < 0)
        return -1;
    if (fileXioInit() < 0)
        return -1;

    /*
     * MMCEDRV must hook SIO2MAN before PAD/MEMCARD modules import it. The
     * bridge then exposes MMCEDRV through the same file-like API used by the
     * asset manager. Normal controller/save clients are loaded afterwards.
     */
    if (LOAD_IRX(sio2man) < 0 ||
        LOAD_IRX(mmcedrv) < 0 ||
        LOAD_IRX(ssb_mmce_stream) < 0)
        return -1;

    if (LOAD_IRX(mcman) < 0 ||
        LOAD_IRX(mtapman) < 0 ||
        LOAD_IRX(padman) < 0 ||
        LOAD_IRX(libsd) < 0)
        return -1;

    ps2_log("IOP: final MMCE game stack ready (MMCEDRV before PAD/MC)");
    return 0;
}

int ps2_iop_load_audio_driver(void)
{
    /*
     * Do not use sdrdrv here. Real-hardware testing proved that the SDR client
     * can poison the shared SIF RPC path and stall later storage traffic.
     * ssb_audio is a tiny purpose-built server exposing only the libsd
     * operations this port needs (init, batched register writes, sample DMA,
     * and optional readback).
     */
    if (ps2_iop_module_loaded("ssb_audio"))
        return 0;

    ps2_log("IOP: starting dedicated ssb_audio server");
    return LOAD_IRX(ssb_audio);
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    char ip_arg[24];

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_MC:
        return 0;

    case PS2_BOOT_BDM:
        /* Generic massN: does not say which transport backs it. USB is by
         * far the common case, so bring it up first; a missing USB driver is
         * not fatal because ps2_iop_load_bdm_fallback_transports() can still
         * add the other local transports without resetting the IOP. */
        if (load_bdm_core() < 0)
            return -1;
        if (LOAD_IRX(usbmass_bd_mini) < 0)
            ps2_log("IOP: USB mass storage unavailable; other BDM transports remain");
        return 0;

    case PS2_BOOT_CDROM:
        return LOAD_IRX(cdvd);

    case PS2_BOOT_USB:
        if (load_bdm_core() < 0 || LOAD_IRX(usbmass_bd_mini) < 0)
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

int ps2_iop_load_bdm_fallback_transports(void)
{
    static int sTier;

    if (ps2_storage_data_device() != PS2_BOOT_BDM)
        return 0;

    /*
     * Match RiptOPL's literal-mass recovery order instead of loading every
     * possible backend at once. USB is already the initial tier. If the exact
     * massN: slot has not reappeared, add MX4SIO alone; only then add the
     * expensive iLink + ATA transports. Nothing here resets the IOP.
     */
    if (sTier == 0)
    {
        sTier = 1;
        ps2_log("IOP: mass recovery tier 2: MX4SIO");
        LOAD_IRX(mx4sio_bd);
        return 1;
    }

    if (sTier == 1)
    {
        sTier = 2;
        ps2_log("IOP: mass recovery tier 3: iLink + ATA");
        if (LOAD_IRX(iLinkman) >= 0)
            LOAD_IRX(IEEE1394_bd);
        if (LOAD_IRX(ps2dev9) >= 0 && LOAD_IRX(ps2atad) >= 0)
            sleep(1);
        return 1;
    }

    return 0;
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
