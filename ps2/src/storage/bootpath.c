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

static PS2BootDevice sDevice = PS2_BOOT_UNKNOWN;
static char sBootDir[BOOT_DIR_MAX] = "host:";
static int sProgressive;

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void ps2_storage_set_boot_path(const char *argv0)
{
    const char *last_sep;
    size_t len;

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
    else if (starts_with(argv0, "mass") || starts_with(argv0, "usb") || starts_with(argv0, "bdm"))
        sDevice = PS2_BOOT_MASS;
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
            snprintf(sBootDir, sizeof(sBootDir), "pfs0:%s", pfs + 5);
        }
        else
        {
            strcpy(sBootDir, "pfs0:/");
        }
    }
    else if (sDevice == PS2_BOOT_MASS && !starts_with(argv0, "mass"))
    {
        /* other launchers' names for USB storage -> our bdmfs "mass:" */
        const char *colon = strchr(argv0, ':');

        snprintf(sBootDir, sizeof(sBootDir), "mass:%s", colon ? colon + 1 : "/");
    }
    else
    {
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
