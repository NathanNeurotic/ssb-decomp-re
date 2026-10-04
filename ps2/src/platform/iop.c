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

enum
{
    PS2_FS_CLIENT_NONE = 0,
    PS2_FS_CLIENT_FILEIO,
    PS2_FS_CLIENT_FILEXIO
};

static int sFsClient;

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

int ps2_iop_rpc_available(uint32_t rpc_id)
{
    SifRpcClientData_t client __attribute__((aligned(64)));
    int attempt;

    memset(&client, 0, sizeof(client));
    for (attempt = 0; attempt < 500; attempt++)
    {
        if (sceSifBindRpc(&client, rpc_id, 0) < 0)
            return 0;
        if (client.server != NULL)
            return 1;
        DelayThread(1000);
    }
    return 0;
}

static int lazy_load_bridge_irx(const char *label, void *buf, unsigned int size)
{
    int result = 0;
    int id;

    /* RiptOPL/wLaunchELF use this same pattern: keep the live device stack,
     * initialize only the loader/heap RPC clients, enable LoadModuleBuffer,
     * then inject the missing bridge module. No IOP reset, no storage-driver
     * reload, and no mount teardown. */
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    id = SifExecModuleBuffer(buf, size, 0, NULL, &result);

    SifExitIopHeap();
    SifLoadFileExit();

    if (id < 0 || result == 1)
    {
        ps2_log("IOP: lazy %s load returned id=%d res=%d", label, id, result);
        return -1;
    }

    ps2_log("IOP: lazy-loaded %s", label);
    return 0;
}

static int device_prefers_iomanx(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_BDM:
    case PS2_BOOT_USB:
    case PS2_BOOT_ATA:
    case PS2_BOOT_MX4SIO:
    case PS2_BOOT_ILINK:
    case PS2_BOOT_UDPBD:
    case PS2_BOOT_UDPFS:
    case PS2_BOOT_HDD:
    case PS2_BOOT_MMCE:
        return 1;
    default:
        return 0;
    }
}

static int activate_filexio_client(void)
{
    if (sFsClient == PS2_FS_CLIENT_FILEXIO)
        return 0;

    /* fileXioInit() has an unbounded bind loop, so only enter it after the
     * bounded service probe proved that the inherited FILEXIO server exists. */
    if (!ps2_iop_rpc_available(FILEXIO_IRX))
        return -1;
    if (fileXioInit() < 0)
        return -1;

    sFsClient = PS2_FS_CLIENT_FILEXIO;
    ps2_log("IOP: filesystem client = fileXio/iomanX");
    return 0;
}

int ps2_iop_prepare_filesystem_client(void)
{
    const uint32_t fileio_rpc = 0x80000001u;
    PS2BootDevice dev = ps2_storage_data_device();
    int have_filexio = ps2_iop_rpc_available(FILEXIO_IRX);
    int have_fileio = ps2_iop_rpc_available(fileio_rpc);

    ps2_log("IOP: fs RPC fileXio=%s FileIO=%s",
            have_filexio ? "yes" : "no",
            have_fileio ? "yes" : "no");

    /* BDM/PFS/MMCE are iomanX filesystems. host:/mc:/cdrom: are commonly
     * exposed through the legacy FileIO/ioman service. Prefer the bridge that
     * matches the inherited device instead of assuming one RPC model for all
     * launchers (PCSX2 is a useful legacy-FileIO control case). */
    if (device_prefers_iomanx(dev) && have_filexio)
        return activate_filexio_client();

    if (have_fileio)
    {
        sFsClient = PS2_FS_CLIENT_FILEIO;
        ps2_log("IOP: filesystem client = FileIO/ioman");
        return 0;
    }

    if (have_filexio)
        return activate_filexio_client();

    sFsClient = PS2_FS_CLIENT_NONE;
    ps2_log("IOP: no inherited filesystem RPC client is currently reachable");
    return -1;
}

int ps2_iop_promote_filesystem_client(void)
{
    const uint32_t loadfile_rpc = 0x80000006u;
    const uint32_t iopheap_rpc = 0x80000003u;
    PS2BootDevice dev = ps2_storage_data_device();

    if (!ps2_storage_requires_iop_preserve() || !device_prefers_iomanx(dev))
        return 0;
    if (sFsClient == PS2_FS_CLIENT_FILEXIO)
        return 0;

    if (ps2_iop_rpc_available(FILEXIO_IRX))
    {
        if (activate_filexio_client() < 0)
            return -1;
        ps2_log("IOP: promoted filesystem client to inherited fileXio");
        return 1;
    }

    /* The storage filesystem itself stays exactly as the launcher left it.
     * If only the EE<->iomanX bridge is missing, add fileXio alone. Do not
     * reload iomanX, BDM, USB, MX4SIO, MMCE, ATA, or any mounted filesystem. */
    if (!ps2_iop_rpc_available(loadfile_rpc) ||
        !ps2_iop_rpc_available(iopheap_rpc))
    {
        ps2_log("IOP: cannot lazy-load fileXio (loadfile/iopheap RPC absent)");
        return -1;
    }

    ps2_log("IOP: DAT not visible through FileIO; adding fileXio bridge only");
    if (lazy_load_bridge_irx("filexio", filexio_irx, size_filexio_irx) < 0)
        return -1;
    if (!ps2_iop_rpc_available(FILEXIO_IRX))
    {
        ps2_log("IOP: fileXio RPC absent after bridge load");
        return -1;
    }
    if (activate_filexio_client() < 0)
        return -1;

    ps2_log("IOP: promoted filesystem client to lazy fileXio");
    return 1;
}

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
    sFsClient = PS2_FS_CLIENT_NONE;

    /* Every child initializes its own EE-side SIF RPC client state. This does
     * not reset or alter the inherited IOP. */
    SifInitRpc(0);

    if (preserve_iop)
    {
        /* Sidecar contract: the launcher already proved this filesystem by
         * loading SSB64.ELF from it. Keep that IOP untouched. Basic POSIX file
         * calls in PS2SDK bind the inherited FileIO RPC lazily, so fileXio is
         * not a prerequisite for opening the adjacent SSB64.DAT. In
         * particular, do not initialize loadfile/iopheap, apply SBV patches,
         * reset the IOP, query BDM slots, or inject storage/bridge IRXs here. */
        ps2_log("IOP: keeping launcher sidecar device stack untouched");
        return;
    }

    /* Explicit cross-device --data mode is the only path that owns the IOP.
     * It starts from a clean state and loads one known stack. */
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

static int pad_rpc_ready(void)
{
    const uint32_t pad1_new = 0x80000100u;
    const uint32_t pad2_new = 0x80000101u;
    const uint32_t pad1_old = 0x8000010Fu;
    const uint32_t pad2_old = 0x8000011Fu;

    return
        (ps2_iop_rpc_available(pad1_new) && ps2_iop_rpc_available(pad2_new)) ||
        (ps2_iop_rpc_available(pad1_old) && ps2_iop_rpc_available(pad2_old));
}

static int sio2_fallback_is_safe(void)
{
    PS2BootDevice dev = ps2_storage_data_device();

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_USB:
    case PS2_BOOT_ATA:
    case PS2_BOOT_ILINK:
    case PS2_BOOT_UDPBD:
    case PS2_BOOT_UDPFS:
    case PS2_BOOT_HDD:
    case PS2_BOOT_CDROM:
        return 1;

    case PS2_BOOT_BDM:
    {
        char driver[32];

        if (!ps2_storage_inherited_bdm_driver(driver, sizeof(driver)))
        {
            ps2_log("IOP: BDM transport unknown; refusing to replace SIO2");
            return 0;
        }

        ps2_log("IOP: inherited BDM transport=%s", driver);
        if (strcmp(driver, "sdc") == 0 || strcmp(driver, "mx4sio") == 0)
            return 0;

        return strcmp(driver, "usb") == 0 ||
               strcmp(driver, "ata") == 0 ||
               strcmp(driver, "sd") == 0 ||
               strcmp(driver, "ilink") == 0 ||
               strcmp(driver, "udp") == 0;
    }

    case PS2_BOOT_MC:
    case PS2_BOOT_MX4SIO:
    case PS2_BOOT_MMCE:
    case PS2_BOOT_UNKNOWN:
    default:
        return 0;
    }
}

int ps2_iop_prepare_runtime_services(void)
{
    /* Storage is already proven at this point because SSB64.DAT opened from
     * the sidecar path. Only controller RPC is mandatory for gameplay.
     *
     * First try to consume whatever pad stack the launcher left behind. If it
     * is absent, try PADMAN alone so launchers with a compatible live SIO2
     * service keep complete ownership of that transport. Only when PADMAN
     * still cannot register do we add SIO2MAN, and only for devices where
     * replacing/adding SIO2 cannot disconnect the active storage path. */
    if (ps2_storage_requires_iop_preserve())
    {
        if (pad_rpc_ready())
        {
            ps2_log("IOP: inherited pad RPC ready");
            return 0;
        }

        ps2_log("IOP: pad RPC absent; trying padman on inherited SIO2");
        if (lazy_load_bridge_irx("padman", padman_irx, size_padman_irx) >= 0 &&
            pad_rpc_ready())
        {
            ps2_log("IOP: pad RPC ready via inherited SIO2");
            return 0;
        }

        if (!sio2_fallback_is_safe())
        {
            ps2_log("IOP: controller RPC missing and SIO2 is storage-owned/unknown");
            return -1;
        }

        /* PS2SDK's reference pad sample loads SIO2MAN before PADMAN. PCSX2
         * host launches commonly arrive without either service, while USB,
         * ATA, iLink, UDP and HDD storage do not depend on SIO2. */
        ps2_log("IOP: adding sio2man + padman controller stack");
        if (lazy_load_bridge_irx("sio2man", sio2man_irx, size_sio2man_irx) < 0)
            return -1;
        if (lazy_load_bridge_irx("padman", padman_irx, size_padman_irx) < 0)
            return -1;

        if (!pad_rpc_ready())
        {
            ps2_log("IOP: pad RPC unavailable after sio2man + padman");
            return -1;
        }

        ps2_log("IOP: pad RPC ready after sio2man + padman");
    }

    return 0;
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
