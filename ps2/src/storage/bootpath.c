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

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PATH_BUF_MAX 256
#define HDD_SOURCE_MAX 128

static PS2BootDevice sLaunchDevice = PS2_BOOT_UNKNOWN;
static PS2BootDevice sDataDevice = PS2_BOOT_UNKNOWN;
static char sLaunchPath[PATH_BUF_MAX] = "";
static char sDataDir[PATH_BUF_MAX] = "host:";
static char sHddMountSource[HDD_SOURCE_MAX] = "";
static int sProgressive;

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
    if (starts_with_ci(path, "mass") || starts_with_ci(path, "usb"))
        return PS2_BOOT_USB;
    if (starts_with_ci(path, "mc"))
        return PS2_BOOT_MC;
    if (starts_with_ci(path, "ata"))
        return PS2_BOOT_ATA;
    if (starts_with_ci(path, "mx4sio"))
        return PS2_BOOT_MX4SIO;
    if (starts_with_ci(path, "ilink"))
        return PS2_BOOT_ILINK;
    if (starts_with_ci(path, "udpbd"))
        return PS2_BOOT_UDPBD;
    if (starts_with_ci(path, "udpfs"))
        return PS2_BOOT_UDPFS;
    if (starts_with_ci(path, "hdd") || starts_with_ci(path, "pfs") ||
        path[0] == '+' || starts_with_ci(path, "__") || strstr(path, ":pfs") != NULL)
        return PS2_BOOT_HDD;
    if (starts_with_ci(path, "mmce"))
        return PS2_BOOT_MMCE;
    if (starts_with_ci(path, "cdrom"))
        return PS2_BOOT_CDROM;

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

static int normalise_usb_path(const char *path, char *out, size_t out_size)
{
    const char *colon = strchr(path, ':');
    int unit = 0;

    if (colon == NULL)
        return 0;

    if (starts_with_ci(path, "mass") && path[4] >= '0' && path[4] <= '9')
        unit = path[4] - '0';
    else if (starts_with_ci(path, "usb") && path[3] >= '0' && path[3] <= '9')
        unit = path[3] - '0';

    snprintf(out, out_size, "mass%d:%s", unit, colon + 1);
    return 1;
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

    pfs = part_start;
    while ((pfs = strcasestr(pfs, "pfs")) != NULL)
    {
        if (pfs > part_start && is_pfs_token(pfs))
            break;
        pfs += 3;
    }

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

    if (dev == PS2_BOOT_HDD)
    {
        if (!normalise_hdd_path(path, tmp, sizeof(tmp), path_is_file))
            return 0;
    }
    else if (dev == PS2_BOOT_USB)
    {
        if (!normalise_usb_path(path, tmp, sizeof(tmp)))
            return 0;
        if (path_is_file)
            strip_filename(tmp);
        else
            ensure_directory_suffix(tmp, sizeof(tmp), dev);
        sHddMountSource[0] = '\0';
    }
    else
    {
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
        if (p[0] == '2' && p[1] == '4' && p[2] == '0' && (p[3] == 'p' || p[3] == 'P'))
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
    case PS2_BOOT_USB: return "usb/mass";
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
