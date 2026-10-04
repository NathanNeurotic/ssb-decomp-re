/*
 * Boot device and filesystem resolution.
 *
 * argv[0] is a launch identity, not necessarily a path that remains readable
 * after an IOP reset. BDM launchers may hand over identities such as usb0:/,
 * mx4sio0:/, ilink0:/ or ata0:/ while bdmfs exposes the mounted filesystem
 * as massN:. Keep transport, filesystem and asset-directory identity separate.
 *
 * A literal massN: remains that exact slot. Typed BDM aliases are resolved to
 * the massN: containing the running ELF; SSB64.DAT is used as fallback proof.
 */
#include <ps2/platform.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BOOT_DIR_MAX 256
#define BOOT_PATH_MAX 512
#define BOOT_NAME_MAX 128
#define BDM_MAX_SLOTS 8

static PS2BootDevice sDevice = PS2_BOOT_UNKNOWN;
static PS2Filesystem sFilesystem = PS2_FS_UNKNOWN;
static char sOriginalPath[BOOT_PATH_MAX];
static char sBootDir[BOOT_DIR_MAX] = "host:";
static char sRelativeDir[BOOT_DIR_MAX];
static char sBootName[BOOT_NAME_MAX];
static char sHddPartition[128];
static int sProgressive;
static int sExplicitSlot = -1;
static int sPreferredSlot = -1;
static int sNeedsResolve;
static int sPreserveIop;

static void normalize_slashes(char *s)
{
    for (; *s != '\0'; s++)
    {
        if (*s == '\\')
            *s = '/';
    }
}

static int prefix_is(const char *token, const char *name)
{
    size_t i, n = strlen(name);

    for (i = 0; i < n; i++)
    {
        if (token[i] == '\0' ||
            tolower((unsigned char)token[i]) != tolower((unsigned char)name[i]))
            return 0;
    }
    return token[n] == '\0' || isdigit((unsigned char)token[n]) || token[n] == '?';
}

static int token_unit(const char *token, const char *stem)
{
    const char *p = token + strlen(stem);
    int unit = -1;

    if (*p == '?')
        return -2;
    if (!isdigit((unsigned char)*p))
        return -1;

    unit = 0;
    while (isdigit((unsigned char)*p))
    {
        unit = unit * 10 + (*p - '0');
        p++;
    }
    return (*p == '\0') ? unit : -1;
}

static void split_relative(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = slash;
    size_t n;

    if (bslash != NULL && (sep == NULL || bslash > sep))
        sep = bslash;

    sRelativeDir[0] = '\0';
    sBootName[0] = '\0';

    if (sep != NULL)
    {
        n = (size_t)(sep - path) + 1;
        if (n >= sizeof(sRelativeDir))
            n = sizeof(sRelativeDir) - 1;
        memcpy(sRelativeDir, path, n);
        sRelativeDir[n] = '\0';
        snprintf(sBootName, sizeof(sBootName), "%s", sep + 1);
    }
    else
    {
        snprintf(sBootName, sizeof(sBootName), "%s", path);
    }
    normalize_slashes(sRelativeDir);
}

static void directory_from_full_path(const char *path)
{
    const char *last = NULL;
    const char *p;
    size_t n;

    for (p = path; *p != '\0'; p++)
    {
        if (*p == '/' || *p == '\\' || *p == ':')
            last = p;
    }
    if (last == NULL)
    {
        sBootDir[0] = '\0';
        return;
    }

    n = (size_t)(last - path) + 1;
    if (n >= sizeof(sBootDir))
        n = sizeof(sBootDir) - 1;
    memcpy(sBootDir, path, n);
    sBootDir[n] = '\0';
}

static const char *find_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle);

    for (; *s != '\0'; s++)
    {
        size_t i;
        for (i = 0; i < n; i++)
        {
            if (s[i] == '\0' ||
                tolower((unsigned char)s[i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == n)
            return s;
    }
    return NULL;
}

static int is_direct_pfs_path(const char *path)
{
    return tolower((unsigned char)path[0]) == 'p' &&
           tolower((unsigned char)path[1]) == 'f' &&
           tolower((unsigned char)path[2]) == 's' &&
           (path[3] == ':' || isdigit((unsigned char)path[3]));
}

static void set_pfs_boot_dir_from_subpath(const char *sub)
{
    while (*sub == ':' || *sub == '/' || *sub == '\\')
        sub++;

    split_relative(sub);
    if (sRelativeDir[0] != '\0')
        snprintf(sBootDir, sizeof(sBootDir), "pfs0:/%s", sRelativeDir);
    else
        snprintf(sBootDir, sizeof(sBootDir), "pfs0:/");
    normalize_slashes(sBootDir);
}

static void parse_hdd_path(const char *path)
{
    const char *first_colon = strchr(path, ':');
    const char *part_start;
    const char *part_end = NULL;
    const char *pfs;
    const char *sub;
    const char *slash;
    const char *bslash;
    const char *colon;
    size_t n;

    sDevice = PS2_BOOT_HDD;
    sFilesystem = PS2_FS_PFS;

    if (is_direct_pfs_path(path))
    {
        /* A launcher-owned pfsN: mount cannot be reconstructed after reset:
         * argv[0] contains no APA partition identity. Preserve the IOP. */
        sPreserveIop = 1;
        directory_from_full_path(path);
        return;
    }

    if (first_colon == NULL)
    {
        directory_from_full_path(path);
        return;
    }

    part_start = first_colon + 1;
    pfs = find_ci(part_start, ":pfs");
    if (pfs != NULL)
    {
        part_end = pfs;
        sub = pfs + 4; /* after ":pfs" */
        while (isdigit((unsigned char)*sub))
            sub++;
        if (*sub == ':')
            sub++;
    }
    else
    {
        colon = strchr(part_start, ':');
        slash = strchr(part_start, '/');
        bslash = strchr(part_start, '\\');
        if (bslash != NULL && (slash == NULL || bslash < slash))
            slash = bslash;

        part_end = colon;
        if (slash != NULL && (part_end == NULL || slash < part_end))
            part_end = slash;
        if (part_end == NULL)
            part_end = part_start + strlen(part_start);

        sub = part_end;
        if (*sub == ':')
            sub++;
    }

    if (part_end > part_start)
    {
        n = (size_t)(part_end - path);
        if (n >= sizeof(sHddPartition))
            n = sizeof(sHddPartition) - 1;
        memcpy(sHddPartition, path, n);
        sHddPartition[n] = '\0';
    }

    set_pfs_boot_dir_from_subpath(sub);
}

static void compose_slot_dir(char *out, size_t out_size, const char *prefix, int slot)
{
    if (sRelativeDir[0] == '\0')
        snprintf(out, out_size, "%s%d:/", prefix, slot);
    else if (sRelativeDir[0] == '/')
        snprintf(out, out_size, "%s%d:%s", prefix, slot, sRelativeDir);
    else
        snprintf(out, out_size, "%s%d:/%s", prefix, slot, sRelativeDir);
    normalize_slashes(out);
}

static int file_exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int slot_has(const char *prefix, int slot, const char *name)
{
    char dir[BOOT_DIR_MAX];
    char path[BOOT_PATH_MAX];

    if (name == NULL || name[0] == '\0')
        return 0;
    compose_slot_dir(dir, sizeof(dir), prefix, slot);
    snprintf(path, sizeof(path), "%s%s", dir, name);
    return file_exists(path);
}

static int choose_slot(const char *prefix, int slot_count, int exact_slot)
{
    int pass, scan, slot, first;

    /* Prefer the actual ELF identity. The asset pack is a fallback for
     * launchers that rewrite argv[0] while handing off. */
    for (pass = 0; pass < 2; pass++)
    {
        for (scan = 0; scan < slot_count; scan++)
        {
            if (exact_slot >= 0)
            {
                slot = exact_slot;
                if (scan != 0)
                    break;
            }
            else
            {
                first = (sPreferredSlot >= 0 && sPreferredSlot < slot_count)
                            ? sPreferredSlot
                            : 0;
                slot = (scan == 0) ? first : (scan <= first ? scan - 1 : scan);
            }

            if ((pass == 0 && slot_has(prefix, slot, sBootName)) ||
                (pass == 1 && slot_has(prefix, slot, "SSB64.DAT")))
            {
                compose_slot_dir(sBootDir, sizeof(sBootDir), prefix, slot);
                sExplicitSlot = slot;
                sNeedsResolve = 0;
                return 1;
            }
        }
    }
    return 0;
}

void ps2_storage_set_boot_path(const char *argv0)
{
    char token[32];
    const char *colon;
    const char *after;
    size_t token_len;
    int unit = -1;

    sDevice = PS2_BOOT_UNKNOWN;
    sFilesystem = PS2_FS_UNKNOWN;
    sOriginalPath[0] = '\0';
    sBootDir[0] = '\0';
    sRelativeDir[0] = '\0';
    sBootName[0] = '\0';
    sHddPartition[0] = '\0';
    sExplicitSlot = -1;
    sPreferredSlot = -1;
    sNeedsResolve = 0;
    sPreserveIop = 0;
    sProgressive = 0;

    if (argv0 == NULL || argv0[0] == '\0')
    {
        sDevice = PS2_BOOT_HOST;
        sFilesystem = PS2_FS_HOST;
        sPreserveIop = 1;
        snprintf(sOriginalPath, sizeof(sOriginalPath), "host:");
        snprintf(sBootDir, sizeof(sBootDir), "host:");
        return;
    }

    snprintf(sOriginalPath, sizeof(sOriginalPath), "%s", argv0);

    {
        const char *p;
        for (p = argv0; p[0] != '\0'; p++)
        {
            if (p[0] == '2' && p[1] == '4' && p[2] == '0' &&
                (p[3] == 'p' || p[3] == 'P'))
                sProgressive = 1;
        }
    }

    colon = strchr(argv0, ':');
    if (colon == NULL)
    {
        directory_from_full_path(argv0);
        return;
    }

    token_len = (size_t)(colon - argv0);
    if (token_len >= sizeof(token))
        token_len = sizeof(token) - 1;
    memcpy(token, argv0, token_len);
    token[token_len] = '\0';
    after = colon + 1;
    split_relative(after);

    if (prefix_is(token, "host"))
    {
        sDevice = PS2_BOOT_HOST;
        sFilesystem = PS2_FS_HOST;
        sPreserveIop = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "mc"))
    {
        sDevice = PS2_BOOT_MC;
        sFilesystem = PS2_FS_MC;
        unit = token_unit(token, "mc");
        if (unit >= 0 && unit < 2)
        {
            sExplicitSlot = unit;
            directory_from_full_path(argv0);
        }
        else
        {
            sNeedsResolve = 1;
            sPreferredSlot = 0;
            snprintf(sBootDir, sizeof(sBootDir), "mc0:/");
        }
    }
    else if (prefix_is(token, "mmce"))
    {
        sDevice = PS2_BOOT_MMCE;
        sFilesystem = PS2_FS_MMCE;
        unit = token_unit(token, "mmce");
        if (unit >= 0 && unit < 2)
        {
            sExplicitSlot = unit;
            directory_from_full_path(argv0);
        }
        else
        {
            sNeedsResolve = 1;
            sPreferredSlot = 0;
            snprintf(sBootDir, sizeof(sBootDir), "mmce0:/");
        }
    }
    else if (prefix_is(token, "mass"))
    {
        sDevice = PS2_BOOT_BDM;
        sFilesystem = PS2_FS_MASS;
        unit = token_unit(token, "mass");
        if (unit >= 0 && unit < BDM_MAX_SLOTS)
        {
            sExplicitSlot = unit;
            sNeedsResolve = 1;
            directory_from_full_path(argv0);
            normalize_slashes(sBootDir);
        }
        else
        {
            sNeedsResolve = 1;
            sPreferredSlot = 0;
            snprintf(sBootDir, sizeof(sBootDir), "mass0:/");
        }
    }
    else if (prefix_is(token, "usb"))
    {
        sDevice = PS2_BOOT_USB;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = token_unit(token, "usb");
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "mx4sio") || prefix_is(token, "mx4") ||
             prefix_is(token, "sdc") || prefix_is(token, "sd"))
    {
        const char *stem = prefix_is(token, "mx4sio") ? "mx4sio"
                           : prefix_is(token, "mx4")   ? "mx4"
                           : prefix_is(token, "sdc")   ? "sdc"
                                                      : "sd";
        sDevice = PS2_BOOT_MX4SIO;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = token_unit(token, stem);
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "ilink"))
    {
        sDevice = PS2_BOOT_ILINK;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = token_unit(token, "ilink");
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "ata"))
    {
        sDevice = PS2_BOOT_ATA;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = token_unit(token, "ata");
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "udpbd") || prefix_is(token, "udp"))
    {
        sDevice = PS2_BOOT_UDPBD;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = prefix_is(token, "udpbd")
                             ? token_unit(token, "udpbd")
                             : token_unit(token, "udp");
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "udpfs"))
    {
        sDevice = PS2_BOOT_UDPFS;
        sFilesystem = PS2_FS_UDPFS;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "hdd") || prefix_is(token, "pfs"))
    {
        parse_hdd_path(argv0);
    }
    else if (prefix_is(token, "cdrom"))
    {
        sDevice = PS2_BOOT_CDROM;
        sFilesystem = PS2_FS_CDROM;
        directory_from_full_path(argv0);
    }
    else if (prefix_is(token, "bdm"))
    {
        sDevice = PS2_BOOT_BDM;
        sFilesystem = PS2_FS_MASS;
        sPreferredSlot = token_unit(token, "bdm");
        sNeedsResolve = 1;
        directory_from_full_path(argv0);
    }
    else
    {
        directory_from_full_path(argv0);
    }
}

PS2BootDevice ps2_storage_boot_device(void)
{
    return sDevice;
}

PS2Filesystem ps2_storage_filesystem(void)
{
    return sFilesystem;
}

int ps2_storage_should_reset_iop(void)
{
    return !sPreserveIop;
}

int ps2_video_progressive(void)
{
    return sProgressive;
}

const char *ps2_storage_original_path(void)
{
    return sOriginalPath;
}

const char *ps2_storage_boot_dir(void)
{
    return sBootDir;
}

const char *ps2_storage_hdd_partition(void)
{
    return sHddPartition;
}

int ps2_storage_resolve_boot_path(void)
{
    if (!sNeedsResolve)
        return 1;

    switch (sFilesystem)
    {
    case PS2_FS_MASS:
        return choose_slot("mass", BDM_MAX_SLOTS, sExplicitSlot);
    case PS2_FS_MC:
        return choose_slot("mc", 2, sExplicitSlot);
    case PS2_FS_MMCE:
        return choose_slot("mmce", 2, sExplicitSlot);
    default:
        sNeedsResolve = 0;
        return 1;
    }
}

const char *ps2_storage_device_name(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_HOST: return "host";
    case PS2_BOOT_MC: return "memory card";
    case PS2_BOOT_MMCE: return "mmce";
    case PS2_BOOT_CDROM: return "cdrom";
    case PS2_BOOT_BDM: return "bdm/mass";
    case PS2_BOOT_USB: return "usb";
    case PS2_BOOT_MX4SIO: return "mx4sio";
    case PS2_BOOT_ILINK: return "iLink";
    case PS2_BOOT_ATA: return "ata/exfat";
    case PS2_BOOT_UDPBD: return "udpbd";
    case PS2_BOOT_UDPFS: return "udpfs";
    case PS2_BOOT_HDD: return "apa/pfs";
    default: return "unknown";
    }
}

void ps2_storage_path(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s%s", sBootDir, name);
}
