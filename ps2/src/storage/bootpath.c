/*
 * Boot device detection.
 *
 * The ELF never hard-codes a device. argv[0] (as passed by uLaunchELF, OPL,
 * wLaunchELF, PS2BBL, ps2link, PCSX2 "Run ELF", ...) tells us where we were
 * started from; the asset pack and other data are looked up next to the ELF.
 *
 *   mass0:/SSB64/ssb64.elf   -> boot dir "mass0:/SSB64/"
 *   host:ssb64.elf           -> boot dir "host:"
 *   mc0:/APPS/ssb64.elf      -> boot dir "mc0:/APPS/"
 *   hdd0:__common:pfs:/x.elf -> boot dir "pfs0:/"   (partition mounted by iop.c)
 *   cdrom0:\SSB64.ELF;1      -> boot dir "cdrom0:\"
 */
#include <ps2/platform.h>

#include <stdio.h>
#include <string.h>

#define BOOT_DIR_MAX 256
#define DATA_PATH_MAX 300

static PS2BootDevice sDevice = PS2_BOOT_UNKNOWN;
static char sBootDir[BOOT_DIR_MAX] = "host:";
static int sProgressive;
static char sDataPath[DATA_PATH_MAX];
static char sBootHddPartition[96];

/* A supplied DAT path is an explicit identity, never a device-search hint. */
int ps2_storage_set_data_path(const char *path)
{
    size_t n;
    sDataPath[0] = '\0';
    if (path == NULL || path[0] == '\0')
        return 1; /* No override: use ELF-adjacent DAT. */
    n = strlen(path);
    if (strchr(path, ':') == NULL || n >= sizeof(sDataPath))
        return 0; /* Absolute PS2 device paths only; no truncation. */
    memcpy(sDataPath, path, n + 1);
    return 1;
}

int ps2_storage_has_data_override(void)
{
    return sDataPath[0] != '\0';
}

int ps2_storage_pack_path(char *out, size_t out_size)
{
    const char *source = sDataPath[0] ? sDataPath : sBootDir;
    size_t n = strlen(source);
    int full_file = 0, count;
    if (out == NULL || out_size == 0 || n == 0)
        return 0;
    /* --data=.../SSB64.DAT is an explicit file; otherwise a directory. */
    if (n >= 9 && strncmp(source + n - 9, "SSB64.DAT", 9) == 0 &&
        (n == 9 || (n >= 10 && (source[n - 10] == '/' || source[n - 10] == '\\'))))
        full_file = 1;
    if (full_file)
        count = snprintf(out, out_size, "%s", source);
    else
        count = snprintf(out, out_size, "%s%sSSB64.DAT", source,
                         source[n - 1] == '/' || source[n - 1] == '\\' || source[n - 1] == ':' ? "" : "/");
    if (count < 0 || (size_t)count >= out_size)
    {
        out[0] = '\0';
        return 0;
    }
    return 1;
}


static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* DATA can be hosted on another device than the ELF. Device selection
 * is explicit; do not scan massN: drives for a matching DAT. */
PS2BootDevice ps2_storage_data_device(void)
{
    const char *path = sDataPath;
    if (!path[0])
        return sDevice;
    if (starts_with(path, "host:"))
        return PS2_BOOT_HOST;
    if (starts_with(path, "mass") || starts_with(path, "usb"))
        return PS2_BOOT_MASS;
    if (starts_with(path, "ata"))
        return PS2_BOOT_ATA;
    if (starts_with(path, "mx4sio") || starts_with(path, "sdc"))
        return PS2_BOOT_MX4SIO;
    if (starts_with(path, "ilink") || starts_with(path, "sd:"))
        return PS2_BOOT_ILINK;
    if (starts_with(path, "udpbd"))
        return PS2_BOOT_UDPBD;
    if (starts_with(path, "udpfs"))
        return PS2_BOOT_UDPFS;
    if (starts_with(path, "mc"))
        return PS2_BOOT_MC;
    if (starts_with(path, "mmce"))
        return PS2_BOOT_MMCE;
    if (starts_with(path, "hdd") || starts_with(path, "pfs"))
        return PS2_BOOT_HDD;
    if (starts_with(path, "cdrom"))
        return PS2_BOOT_CDROM;
    return PS2_BOOT_UNKNOWN;
}

const char *ps2_storage_boot_hdd_partition(void)
{
    return sBootHddPartition;
}

void ps2_storage_set_boot_path(const char *argv0)
{
    const char *last_sep;
    size_t len;

    /* This may be called again after a different launcher handoff. */
    sProgressive = 0;
    sBootHddPartition[0] = '\0';

    if (argv0 == NULL || argv0[0] == '\0')
    {
        /* PCSX2 fast-boot of an ELF without argv: host: is the best guess. */
        sDevice = PS2_BOOT_HOST;
        strcpy(sBootDir, "host:");
        return;
    }

    /* "..._240p.elf" (any case) selects 240p output; the default is 480i,
     * which every TV accepts. The ELF name is the only setting that is
     * known before any storage driver is up. */
    {
        const char *p;

        for (p = argv0; p[0] != '\0'; p++)
        {
            if (p[0] == '2' && p[1] == '4' && p[2] == '0' && (p[3] == 'p' || p[3] == 'P'))
            {
                sProgressive = 1;
            }
        }
    }

    if (starts_with(argv0, "host"))
        sDevice = PS2_BOOT_HOST;
    else if (starts_with(argv0, "mass") || starts_with(argv0, "usb"))
        sDevice = PS2_BOOT_MASS;
    else if (starts_with(argv0, "ata"))
        sDevice = PS2_BOOT_ATA;
    else if (starts_with(argv0, "mx4sio") || starts_with(argv0, "sdc"))
        sDevice = PS2_BOOT_MX4SIO;
    else if (starts_with(argv0, "ilink") || starts_with(argv0, "sd:"))
        sDevice = PS2_BOOT_ILINK;
    else if (starts_with(argv0, "udpbd"))
        sDevice = PS2_BOOT_UDPBD;
    else if (starts_with(argv0, "udpfs"))
        sDevice = PS2_BOOT_UDPFS;
    else if (starts_with(argv0, "mc"))
        sDevice = PS2_BOOT_MC;
    else if (starts_with(argv0, "hdd") || starts_with(argv0, "pfs"))
        sDevice = PS2_BOOT_HDD;
    else if (starts_with(argv0, "mmce"))
        sDevice = PS2_BOOT_MMCE;
    else if (starts_with(argv0, "cdrom"))
        sDevice = PS2_BOOT_CDROM;
    else
        sDevice = PS2_BOOT_UNKNOWN;

    if (sDevice == PS2_BOOT_HDD && starts_with(argv0, "hdd"))
    {
        /* "hdd0:PARTITION:pfs:/path/file.elf" -> "pfs0:/path/" (iop.c mounts
         * the partition on pfs0: before any file access). */
        const char *pfs = strstr(argv0, ":pfs:");

        if (pfs != NULL)
        {
            /* Mount only the actual APA partition given by the launcher. */
            len = (size_t)(pfs - argv0);
            if (len > 5 && len < sizeof(sBootHddPartition))
            {
                memcpy(sBootHddPartition, argv0, len);
                sBootHddPartition[len] = '\0';
            }
            snprintf(sBootDir, sizeof(sBootDir), "pfs0:%s", pfs + 5);
        }
        else
        {
            strcpy(sBootDir, "pfs0:/");
        }
    }
    else
    {
        /* Preserve the launcher-supplied device identifier byte-for-byte.
         * usbN:/ and bdmN:/ are NOT proven aliases of massN: and converting
         * them silently can pick the wrong device in a multi-USB setup.
         * Backend compatibility must be decided separately from path parsing. */
        strncpy(sBootDir, argv0, sizeof(sBootDir) - 1);
        sBootDir[sizeof(sBootDir) - 1] = '\0';
    }

    /* Strip the file name: keep everything up to the last '/', '\\' or ':'. */
    last_sep = NULL;
    {
        const char *p;

        for (p = sBootDir; *p != '\0'; p++)
        {
            if (*p == '/' || *p == '\\' || *p == ':')
            {
                last_sep = p;
            }
        }
    }
    if (last_sep != NULL)
    {
        len = (size_t)(last_sep - sBootDir) + 1;
        sBootDir[len] = '\0';
    }
}

PS2BootDevice ps2_storage_boot_device(void)
{
    return sDevice;
}

int ps2_video_progressive(void)
{
    return sProgressive;
}

const char *ps2_storage_boot_dir(void)
{
    return sBootDir;
}

const char *ps2_storage_device_name(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_HOST: return "host";
    case PS2_BOOT_MASS: return "mass";
    case PS2_BOOT_ATA: return "ata";
    case PS2_BOOT_MX4SIO: return "mx4sio";
    case PS2_BOOT_ILINK: return "ilink";
    case PS2_BOOT_UDPBD: return "udpbd";
    case PS2_BOOT_UDPFS: return "udpfs";
    case PS2_BOOT_MC: return "memory card";
    case PS2_BOOT_HDD: return "hdd";
    case PS2_BOOT_MMCE: return "mmce";
    case PS2_BOOT_CDROM: return "cdrom";
    default: return "unknown";
    }
}

void ps2_storage_path(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s%s", sBootDir, name);
}
