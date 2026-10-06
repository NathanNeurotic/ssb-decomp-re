/*
 * Boot/data device detection and path normalisation.
 *
 * argv[0] tells us where the ELF was launched from.  That is not necessarily
 * where the 25 MiB SSB64.DAT lives: an optional --data=<directory> argument
 * can point at any supported filesystem.  Keeping launch and data devices
 * separate also lets us preserve host: (ps2link/PCSX2) while loading a real
 * storage stack for assets.
 *
 * Canonical examples:
 *   usb:/SSB64/ssb64.elf              -> mass0:/SSB64/
 *   mass1:/SSB64/ssb64.elf            -> mass1:/SSB64/
 *   mx4sio:/SSB64/ssb64.elf           -> mx4sio:/SSB64/
 *   hdd0:+OPL:pfs:/SSB64/ssb64.elf    -> pfs0:/SSB64/
 *                                         mount source hdd0:+OPL
 *   udpfs:/SSB64/ssb64.elf            -> udpfs:/SSB64/
 *
 * A bare bdm: path is deliberately NOT guessed: it does not identify which
 * transport must be reconstructed after an IOP reset.
 */
#include <ps2/platform.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <usbhdfsd-common.h>

#define PATH_BUF_MAX 256
#define HDD_SOURCE_MAX 128

static PS2BootDevice sLaunchDevice = PS2_BOOT_UNKNOWN;
static PS2BootDevice sDataDevice = PS2_BOOT_UNKNOWN;
static char sLaunchPath[PATH_BUF_MAX] = "";
static char sDataDir[PATH_BUF_MAX] = "host:";
static char sHddMountSource[HDD_SOURCE_MAX] = "";
static int sProgressive;
static int sDataNeedsExistingIop;
static int sDataNeedsBdmResolve;
static int sBdmDriverOrdinal = -1;

static int starts_with_ci(const char *s, const char *prefix)
{
    return s != NULL && strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static PS2BootDevice detect_device(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return PS2_BOOT_UNKNOWN;
    if (starts_with_ci(path, "host"))
        return PS2_BOOT_HOST;
    if (starts_with_ci(path, "mass"))
        return PS2_BOOT_BDM;
    if (starts_with_ci(path, "usb"))
        return PS2_BOOT_USB;
    if (starts_with_ci(path, "mc"))
        return PS2_BOOT_MC;
    if (starts_with_ci(path, "ata"))
        return PS2_BOOT_ATA;
    if (starts_with_ci(path, "mx4sio") || starts_with_ci(path, "mx4:") ||
        starts_with_ci(path, "mx4/"))
        return PS2_BOOT_MX4SIO;
    if (starts_with_ci(path, "ilink"))
        return PS2_BOOT_ILINK;
    if (starts_with_ci(path, "udpbd"))
        return PS2_BOOT_UDPBD;
    if (starts_with_ci(path, "udpfs"))
        return PS2_BOOT_UDPFS;
    if (starts_with_ci(path, "mmce"))
        return PS2_BOOT_MMCE;
    if (starts_with_ci(path, "cdrom"))
        return PS2_BOOT_CDROM;
    if (starts_with_ci(path, "hdd") || starts_with_ci(path, "pfs") ||
        path[0] == '+' || starts_with_ci(path, "__") || strstr(path, ":pfs") != NULL)
        return PS2_BOOT_HDD;

    /* "bdm:" alone is intentionally ambiguous (USB/MX4SIO/iLink/ATA/UDPBD). */
    return PS2_BOOT_UNKNOWN;
}

static void strip_filename(char *path)
{
    const char *last = NULL;
    const char *p;
    size_t len;

    for (p = path; *p != '\0'; p++)
    {
        if (*p == '/' || *p == '\\' || *p == ':')
            last = p;
    }
    if (last == NULL)
    {
        path[0] = '\0';
        return;
    }
    len = (size_t)(last - path) + 1;
    path[len] = '\0';
}

static void ensure_directory_suffix(char *path, size_t size, PS2BootDevice dev)
{
    size_t len = strlen(path);
    char sep = (dev == PS2_BOOT_CDROM) ? '\\' : '/';

    if (len == 0 || path[len - 1] == ':' || path[len - 1] == '/' || path[len - 1] == '\\')
        return;
    if (len + 1 < size)
    {
        path[len] = sep;
        path[len + 1] = '\0';
    }
}

static int is_pfs_token(const char *p)
{
    const char *q;

    if (!starts_with_ci(p, "pfs"))
        return 0;
    q = p + 3;
    while (*q >= '0' && *q <= '9')
        q++;
    return *q == ':' || *q == '/' || *q == '\\' || *q == '\0';
}

static const char *find_pfs_token(const char *s)
{
    const char *p;

    for (p = s; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; p++)
    {
        if ((p[0] == 'p' || p[0] == 'P') &&
            (p[1] == 'f' || p[1] == 'F') &&
            (p[2] == 's' || p[2] == 'S') &&
            is_pfs_token(p))
            return p;
    }
    return NULL;
}

/* Resolve a canonical HDD/PFS runtime path and remember which APA partition
 * must be mounted on pfs0:.  A bare pfs0: path can be used only when the
 * caller preserved an already-mounted IOP; after our normal reset there is
 * no way to reconstruct the source partition, so iop.c rejects it. */
static int normalise_hdd_path(const char *path, char *out, size_t out_size, int path_is_file)
{
    char hdd_prefix[8] = "hdd0:";
    const char *p = path;
    const char *part_start;
    const char *part_end = NULL;
    const char *sub;
    const char *colon;
    const char *slash;
    const char *bslash;
    const char *pfs;
    size_t part_len;

    sHddMountSource[0] = '\0';

    if (starts_with_ci(p, "pfs"))
    {
        /* A bare pfsN: path contains no APA partition identity. It is only
         * usable while preserving the launcher's already-mounted IOP state. */
        sDataNeedsExistingIop = 1;
        strncpy(out, p, out_size - 1);
        out[out_size - 1] = '\0';
        if (path_is_file)
            strip_filename(out);
        else
            ensure_directory_suffix(out, out_size, PS2_BOOT_HDD);
        return 1;
    }

    if (starts_with_ci(p, "hdd"))
    {
        colon = strchr(p, ':');
        if (colon == NULL || (size_t)(colon - p + 1) >= sizeof(hdd_prefix))
            return 0;
        memcpy(hdd_prefix, p, (size_t)(colon - p + 1));
        hdd_prefix[colon - p + 1] = '\0';
        p = colon + 1;
    }

    while (*p == '/' || *p == '\\')
        p++;
    part_start = p;

    colon = strchr(part_start, ':');
    slash = strchr(part_start, '/');
    bslash = strchr(part_start, '\\');
    if (bslash != NULL && (slash == NULL || bslash < slash))
        slash = bslash;

    pfs = find_pfs_token(part_start);
    if (pfs == part_start)
        pfs = NULL;

    if (colon != NULL)
        part_end = colon;
    if (slash != NULL && (part_end == NULL || slash < part_end))
        part_end = slash;
    if (pfs != NULL && (part_end == NULL || pfs < part_end))
        part_end = pfs;
    if (part_end == NULL)
        part_end = part_start + strlen(part_start);

    part_len = (size_t)(part_end - part_start);
    if (part_len == 0 || strlen(hdd_prefix) + part_len >= sizeof(sHddMountSource))
        return 0;

    snprintf(sHddMountSource, sizeof(sHddMountSource), "%s%.*s",
             hdd_prefix, (int)part_len, part_start);

    sub = part_end;
    if (*sub == ':')
        sub++;
    if (starts_with_ci(sub, "pfs"))
    {
        sub += 3;
        while (*sub >= '0' && *sub <= '9')
            sub++;
        if (*sub == ':')
            sub++;
    }

    if (*sub == '\0')
        snprintf(out, out_size, "pfs0:/");
    else if (*sub == '/' || *sub == '\\')
        snprintf(out, out_size, "pfs0:%s", sub);
    else
        snprintf(out, out_size, "pfs0:/%s", sub);

    /* PFS accepts forward slashes consistently. */
    {
        char *q;
        for (q = out; *q != '\0'; q++)
            if (*q == '\\')
                *q = '/';
    }

    if (path_is_file)
        strip_filename(out);
    else
        ensure_directory_suffix(out, out_size, PS2_BOOT_HDD);
    return 1;
}

static int set_data_location(const char *path, int path_is_file)
{
    PS2BootDevice dev = detect_device(path);
    char tmp[PATH_BUF_MAX];

    if (dev == PS2_BOOT_UNKNOWN)
        return 0;

    sBdmDriverOrdinal = -1;

    if (dev == PS2_BOOT_HDD)
    {
        sDataNeedsExistingIop = 0;
        sDataNeedsBdmResolve = 0;
        if (!normalise_hdd_path(path, tmp, sizeof(tmp), path_is_file))
            return 0;
    }
    else if (dev == PS2_BOOT_BDM)
    {
        /*
         * OPL/RiptOPL expose every BDM transport as connection-order massN:.
         * The name alone does NOT mean USB.  Do not inherit that launcher IOP
         * indefinitely and do not guess USB: boot.c performs the same
         * driver-name discovery used by launcHER, then rebuilds exactly the
         * discovered transport stack.
         */
        sDataNeedsExistingIop = 0;
        sDataNeedsBdmResolve = 0;
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        if (path_is_file)
            strip_filename(tmp);
        else
            ensure_directory_suffix(tmp, sizeof(tmp), dev);
        sHddMountSource[0] = '\0';
    }
    else if (dev == PS2_BOOT_USB)
    {
        sDataNeedsExistingIop = 0;
        /* usbN: is a typed transport identity on older BDM stacks. Resolve
         * it to the matching massN: filesystem after the USB driver loads. */
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        sDataNeedsBdmResolve = 1;
        if (path_is_file)
            strip_filename(tmp);
        else
            ensure_directory_suffix(tmp, sizeof(tmp), dev);
        sHddMountSource[0] = '\0';
    }
    else
    {
        sDataNeedsExistingIop = (dev == PS2_BOOT_HOST);
        sDataNeedsBdmResolve = (dev == PS2_BOOT_ATA || dev == PS2_BOOT_MX4SIO ||
                                dev == PS2_BOOT_ILINK || dev == PS2_BOOT_UDPBD);
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        if (path_is_file)
            strip_filename(tmp);
        else
            ensure_directory_suffix(tmp, sizeof(tmp), dev);
        sHddMountSource[0] = '\0';
    }

    sDataDevice = dev;
    strncpy(sDataDir, tmp, sizeof(sDataDir) - 1);
    sDataDir[sizeof(sDataDir) - 1] = '\0';
    return 1;
}

void ps2_storage_set_boot_path(const char *argv0)
{
    const char *p;

    sProgressive = 0;
    sHddMountSource[0] = '\0';
    sDataNeedsExistingIop = 0;
    sDataNeedsBdmResolve = 0;

    if (argv0 == NULL || argv0[0] == '\0')
    {
        sLaunchDevice = PS2_BOOT_HOST;
        sDataDevice = PS2_BOOT_HOST;
        strcpy(sLaunchPath, "host:");
        strcpy(sDataDir, "host:");
        return;
    }

    strncpy(sLaunchPath, argv0, sizeof(sLaunchPath) - 1);
    sLaunchPath[sizeof(sLaunchPath) - 1] = '\0';

    for (p = argv0; p[0] != '\0'; p++)
    {
        if (p[1] != '\0' && p[2] != '\0' && p[3] != '\0' &&
            p[0] == '2' && p[1] == '4' && p[2] == '0' && (p[3] == 'p' || p[3] == 'P'))
            sProgressive = 1;
    }

    sLaunchDevice = detect_device(argv0);
    if (!set_data_location(argv0, 1))
    {
        sDataDevice = PS2_BOOT_UNKNOWN;
        sDataDir[0] = '\0';
    }
}

int ps2_storage_set_data_path(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    return set_data_location(path, 0);
}

PS2BootDevice ps2_storage_launch_device(void)
{
    return sLaunchDevice;
}

PS2BootDevice ps2_storage_data_device(void)
{
    return sDataDevice;
}

PS2BootDevice ps2_storage_boot_device(void)
{
    return sDataDevice;
}

const char *ps2_storage_launch_path(void)
{
    return sLaunchPath;
}

const char *ps2_storage_hdd_mount_source(void)
{
    return sHddMountSource;
}

int ps2_storage_requires_iop_preserve(void)
{
    return sDataNeedsExistingIop;
}

static int bdm_driver_matches(PS2BootDevice dev, const char *driver)
{
    if (driver == NULL || driver[0] == '\0')
        return 0;

    switch (dev)
    {
    case PS2_BOOT_USB:
        return strcmp(driver, "usb") == 0;
    case PS2_BOOT_ATA:
        return strcmp(driver, "ata") == 0;
    case PS2_BOOT_MX4SIO:
        return strcmp(driver, "sdc") == 0 || strcmp(driver, "mx4sio") == 0;
    case PS2_BOOT_ILINK:
        return strcmp(driver, "sd") == 0 || strcmp(driver, "ilink") == 0;
    case PS2_BOOT_UDPBD:
        return strcmp(driver, "udp") == 0;
    default:
        return 0;
    }
}

static PS2BootDevice bdm_driver_device(const char *driver)
{
    if (driver == NULL)
        return PS2_BOOT_UNKNOWN;
    if (strcmp(driver, "usb") == 0)
        return PS2_BOOT_USB;
    if (strcmp(driver, "ata") == 0)
        return PS2_BOOT_ATA;
    if (strcmp(driver, "sdc") == 0 || strcmp(driver, "mx4sio") == 0)
        return PS2_BOOT_MX4SIO;
    if (strcmp(driver, "sd") == 0 || strcmp(driver, "ilink") == 0)
        return PS2_BOOT_ILINK;
    if (strcmp(driver, "udp") == 0)
        return PS2_BOOT_UDPBD;
    return PS2_BOOT_UNKNOWN;
}

static int mass_slot_driver(int slot, char *driver, size_t driver_size)
{
    char root[16];
    int fd;
    int dfd;
    int io;

    snprintf(root, sizeof(root), "mass%d:/", slot);

    /*
     * bdmfs_fatfs creates/mounts massN lazily.  This ordering is important on
     * real hardware: asking fileXioDopen/ioctl for an untouched slot can fail
     * even though the backing BDM device is present.  RiptOPL/launcHER probe
     * the filesystem first for the same reason.
     */
    fd = open(root, O_DIRECTORY | O_RDONLY);
    if (fd >= 0)
        close(fd);

    dfd = fileXioDopen(root);
    if (dfd < 0)
        return 0;

    memset(driver, 0, driver_size);
    io = fileXioIoctl2(dfd, USBMASS_IOCTL_GET_DRIVERNAME,
                       NULL, 0, driver, driver_size - 1);
    fileXioDclose(dfd);
    return io >= 0 && driver[0] != '\0';
}

static void mass_build_dir(char *out, size_t out_size, int slot, const char *relative)
{
    if (relative[0] == '/' || relative[0] == '\\')
        snprintf(out, out_size, "mass%d:%s", slot, relative);
    else
        snprintf(out, out_size, "mass%d:/%s", slot, relative);
    ensure_directory_suffix(out, out_size, sDataDevice);
}

/*
 * Called only while the temporary all-BDM discovery stack is live.
 * This mirrors launcHER's resolveMassPath(): massN: is a connection-order
 * alias, so locate the actual volume by its relative path, ask bdmfs which
 * driver owns that slot, and remember the driver's ordinal for the clean
 * exact-stack rebuild that follows.
 */
int ps2_storage_discover_mass_device(const char *probe_name)
{
    char relative[PATH_BUF_MAX];
    const char *colon;
    int requested = 0;
    int pass;

    if (sDataDevice != PS2_BOOT_BDM || probe_name == NULL || probe_name[0] == '\0')
        return 0;

    colon = strchr(sDataDir, ':');
    if (colon == NULL)
        return 0;
    snprintf(relative, sizeof(relative), "%s", colon + 1);
    if (relative[0] == '\0')
        snprintf(relative, sizeof(relative), "/");

    if (starts_with_ci(sDataDir, "mass"))
    {
        const char *p = sDataDir + 4;
        if (*p >= '0' && *p <= '9')
            requested = *p - '0';
    }

    /* Prefer the slot handed to us. If driver load order changed after the
     * reset, fall back to every mass slot and identify our volume by the same
     * relative SSB64.DAT path. */
    for (pass = -1; pass < 10; pass++)
    {
        int slot = (pass < 0) ? requested : pass;
        char dir[PATH_BUF_MAX];
        char probe[PATH_BUF_MAX + 64];
        char driver[32];
        PS2BootDevice dev;
        int fd;
        int ordinal = 0;
        int earlier;

        if (pass >= 0 && slot == requested)
            continue;

        mass_build_dir(dir, sizeof(dir), slot, relative);
        snprintf(probe, sizeof(probe), "%s%s", dir, probe_name);
        fd = open(probe, O_RDONLY);
        if (fd < 0)
            continue;
        close(fd);

        if (!mass_slot_driver(slot, driver, sizeof(driver)))
            continue;
        dev = bdm_driver_device(driver);
        if (dev == PS2_BOOT_UNKNOWN || dev == PS2_BOOT_UDPBD)
            continue; /* Generic UDPBD discovery remains explicit-only, like launcHER. */

        for (earlier = 0; earlier < slot; earlier++)
        {
            char earlier_driver[32];
            if (mass_slot_driver(earlier, earlier_driver, sizeof(earlier_driver)) &&
                bdm_driver_matches(dev, earlier_driver))
                ordinal++;
        }

        sDataDevice = dev;
        sDataNeedsExistingIop = 0;
        sDataNeedsBdmResolve = 1;
        sBdmDriverOrdinal = ordinal;
        ps2_log("storage: generic mass%d resolved as %s ordinal %d (driver=%s)",
                slot, ps2_storage_device_name(dev), ordinal, driver);
        return 1;
    }

    return 0;
}

int ps2_storage_resolve_data_root(const char *probe_name)
{
    char relative[PATH_BUF_MAX];
    const char *colon;
    int slot;
    int ordinal_seen = 0;

    if (!sDataNeedsBdmResolve)
        return 1;
    if (probe_name == NULL || probe_name[0] == '\0')
        return 0;

    colon = strchr(sDataDir, ':');
    if (colon == NULL)
        return 0;
    snprintf(relative, sizeof(relative), "%s", colon + 1);
    if (relative[0] == '\0')
        snprintf(relative, sizeof(relative), "/");

    /*
     * First preserve the per-driver ordinal learned during generic massN:
     * discovery (important with two USB sticks). If enumeration changed in a
     * way that invalidates it, the second pass below still finds the volume by
     * driver + relative path.
     */
    if (sBdmDriverOrdinal >= 0)
    {
        for (slot = 0; slot < 10; slot++)
        {
            char driver[32];
            char dir[PATH_BUF_MAX];
            char probe[PATH_BUF_MAX + 64];
            int fd;

            if (!mass_slot_driver(slot, driver, sizeof(driver)) ||
                !bdm_driver_matches(sDataDevice, driver))
                continue;
            if (ordinal_seen++ != sBdmDriverOrdinal)
                continue;

            mass_build_dir(dir, sizeof(dir), slot, relative);
            snprintf(probe, sizeof(probe), "%s%s", dir, probe_name);

            /*
             * The ordinal identifies the physical device, but do not commit
             * the path until the actual pack opens on its freshly mounted
             * filesystem. If enumeration changed, fall through to the
             * driver+path scan below.
             */
            fd = open(probe, O_RDONLY);
            if (fd >= 0)
            {
                close(fd);
                snprintf(sDataDir, sizeof(sDataDir), "%s", dir);
                sDataNeedsBdmResolve = 0;
                ps2_log("storage: %s ordinal %d resolved to %s",
                        ps2_storage_device_name(sDataDevice), sBdmDriverOrdinal, sDataDir);
                return 1;
            }
            break;
        }
    }

    for (slot = 0; slot < 10; slot++)
    {
        char dir[PATH_BUF_MAX];
        char probe[PATH_BUF_MAX + 64];
        char driver[32];
        int fd;

        mass_build_dir(dir, sizeof(dir), slot, relative);
        snprintf(probe, sizeof(probe), "%s%s", dir, probe_name);

        /*
         * Restore the original proven ordering: opening the actual pack first
         * lazy-mounts FatFs and proves this is the right volume. Only after
         * that succeeds is it safe/useful to query the slot's driver token.
         * PR #16 accidentally reversed these two operations after the clean
         * exact-device reset, so a moved mass slot could never be discovered.
         */
        fd = open(probe, O_RDONLY);
        if (fd < 0)
            continue;
        close(fd);

        if (!mass_slot_driver(slot, driver, sizeof(driver)) ||
            !bdm_driver_matches(sDataDevice, driver))
            continue;

        snprintf(sDataDir, sizeof(sDataDir), "%s", dir);
        sDataNeedsBdmResolve = 0;
        ps2_log("storage: %s resolved to %s (driver=%s)",
                ps2_storage_device_name(sDataDevice), sDataDir, driver);
        return 1;
    }

    return 0;
}

int ps2_video_progressive(void)
{
    return sProgressive;
}

const char *ps2_storage_boot_dir(void)
{
    return sDataDir;
}

const char *ps2_storage_device_name(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_HOST: return "host";
    case PS2_BOOT_BDM: return "bdm/mass";
    case PS2_BOOT_USB: return "usb";
    case PS2_BOOT_MC: return "memory card";
    case PS2_BOOT_ATA: return "ata-bdm";
    case PS2_BOOT_MX4SIO: return "mx4sio";
    case PS2_BOOT_ILINK: return "ilink";
    case PS2_BOOT_UDPBD: return "udpbd";
    case PS2_BOOT_UDPFS: return "udpfs";
    case PS2_BOOT_HDD: return "apa/pfs";
    case PS2_BOOT_MMCE: return "mmce";
    case PS2_BOOT_CDROM: return "cdrom";
    default: return "unknown";
    }
}

void ps2_storage_path(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s%s", sDataDir, name);
}
