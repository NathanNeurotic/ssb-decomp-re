/*
 * IOP bring-up for real launch/data devices.
 *
 * The launch device and the data device are intentionally separate.  host:
 * keeps the ps2link/PCSX2 IOP alive; every other launch starts from a clean
 * IOP and reconstructs only the stack required by the selected data device.
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

/* Generic massN:/bare-pfs launches must keep the launcher's IOP alive.
 * Re-executing an IRX that is already resident is not a harmless no-op on
 * all real IOP stacks: some versions can block in module startup/RPC setup.
 * Query the resident module list first and only inject pieces that are
 * genuinely absent. */
static int iop_module_present(const char *name1, const char *name2)
{
    int id;

    if (name1 != NULL)
    {
        id = SifSearchModuleByName(name1);
        if (id >= 0)
            return 1;
    }
    if (name2 != NULL)
    {
        id = SifSearchModuleByName(name2);
        if (id >= 0)
            return 1;
    }
    return 0;
}

static int load_irx_if_missing(const char *label, void *buf, unsigned int size,
                               const char *module_name1, const char *module_name2)
{
    if (iop_module_present(module_name1, module_name2))
    {
        ps2_log("IOP: keeping inherited %s", label);
        return 0;
    }
    return load_irx(label, buf, size, NULL, 0);
}

#define LOAD_IRX_IF_MISSING(name, mod1, mod2) \
    load_irx_if_missing(#name, name##_irx, size_##name##_irx, mod1, mod2)

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
    ps2_boot_stage("IOP: SIF RPC ready", 0x800000);

    /* host:, generic massN: and bare pfsN: data paths depend on services or
     * mounts owned by the launcher. Everything else is rebuilt from a known
     * IOP state. */
    if (!preserve_iop)
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
    ps2_boot_stage("IOP: loadfile + heap ready", 0x804000);

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    if (sIopWasReset)
        sbv_patch_fileio();
    ps2_boot_stage("IOP: patches ready", 0x808000);

    if (preserve_iop)
    {
        /* Do not blindly start a second copy of launcher-owned modules.
         * massN: is specifically the case that exposed this on hardware:
         * the solid purple/pink stage screen meant we entered ps2_iop_init()
         * but never returned from duplicate module/RPC startup. */
        LOAD_IRX_IF_MISSING(iomanx, "IO/File_Manager", "IOX/File_Manager");
        LOAD_IRX_IF_MISSING(filexio, "IOX/File_Manager_Rpc", NULL);
    }
    else
    {
        LOAD_IRX(iomanx);
        LOAD_IRX(filexio);
    }

    /* Bind the EE client whether fileXio was inherited or loaded above. */
    fileXioInit();
    ps2_boot_stage("IOP: file RPC ready", 0x008000);

    if (preserve_iop)
    {
        LOAD_IRX_IF_MISSING(sio2man, "sio2man", NULL);
        LOAD_IRX_IF_MISSING(mtapman, "multitap_manager", NULL);
        LOAD_IRX_IF_MISSING(padman, "padman", NULL);
        LOAD_IRX_IF_MISSING(mcman, "mcman_cex", "mcman");
        LOAD_IRX_IF_MISSING(mcserv, "mcserv", NULL);
        LOAD_IRX_IF_MISSING(libsd, "freesd", "libsd");
        LOAD_IRX_IF_MISSING(sdr, "sdr_driver", NULL);
    }
    else
    {
        LOAD_IRX(sio2man);
        LOAD_IRX(mtapman);
        LOAD_IRX(padman);
        LOAD_IRX(mcman);
        LOAD_IRX(mcserv);
        LOAD_IRX(libsd);
        LOAD_IRX(sdr);
    }

    ps2_boot_stage("IOP: base modules ready", 0x008080);
    ps2_log("IOP: %s, %d newly loaded base modules",
            sIopWasReset ? "reset" : "kept (inherited filesystem)", sLoadedCount);
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    char ip_arg[24];

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
