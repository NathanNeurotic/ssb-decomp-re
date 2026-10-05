/*
 * Minimal blocking file API used by the asset manager.
 *
 * Uses the ps2sdk newlib port's POSIX calls. The IOP handoff selects the
 * inherited filesystem RPC first: legacy FileIO/ioman for host-like paths, or
 * fileXio/iomanX for BDM/PFS/MMCE paths. fileXioInit() switches libcglue to
 * its iomanX-backed operations without replacing the launcher's device
 * drivers or mounts.
 */
#include <ps2/platform.h>

#include <delaythread.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int ps2_file_open_read(const char *path)
{
    int fd = open(path, O_RDONLY);

    /* ISO9660 paths on real hardware commonly require the ;1 version suffix,
     * while PCSX2 and some launchers accept the unversioned spelling. Keep
     * both forms so the same asset path works in either environment. */
    if (fd < 0 && path != NULL && strncasecmp(path, "cdrom", 5) == 0)
    {
        const char *leaf = strrchr(path, '\\');
        char versioned[384];

        if (leaf == NULL)
            leaf = strrchr(path, '/');
        if ((leaf == NULL || strchr(leaf, ';') == NULL) &&
            strlen(path) + 2 < sizeof(versioned))
        {
            snprintf(versioned, sizeof(versioned), "%s;1", path);
            fd = open(versioned, O_RDONLY);
        }
    }
    return fd;
}

int ps2_file_read(int fd, void *dst, uint32_t size)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;
    PS2BootDevice dev = ps2_storage_data_device();
    off_t base_pos = -1;

    /* MMCE runtime reads share SIO2 with PADMAN. Capture the expected file
     * position once so a timed-out MMCEMAN transaction can be resumed exactly
     * instead of turning a recoverable bus collision into a fatal short read. */
    if (dev == PS2_BOOT_MMCE)
        base_pos = lseek(fd, 0, SEEK_CUR);

    /* Large reads are split so a single request never blocks too long. */
    while (done < size)
    {
        uint32_t max_chunk = 0x10000u;
        uint32_t chunk;
        int n;

        /* PS2SDK's USB mass driver already caps SCSI requests at 128 sectors
         * (64 KiB) because some real drives freeze above that range. Staying
         * comfortably below the transport ceiling is more important than
         * shaving a few RPCs from a 25 MiB asset pack, especially on USB 1.1.
         * Use the same conservative size for physical BDM transports. */
        if (dev == PS2_BOOT_MMCE)
        {
            /* MMCEMAN owns the SIO2 lock for an entire read request. Keep each
             * request to one 2 KiB MMCE sector so PADMAN gets frequent chances
             * to run between filesystem transactions. */
            max_chunk = 0x800u; /* 2 KiB */
        }
        else if (dev == PS2_BOOT_BDM || dev == PS2_BOOT_USB ||
                 dev == PS2_BOOT_ATA || dev == PS2_BOOT_MX4SIO ||
                 dev == PS2_BOOT_ILINK || dev == PS2_BOOT_UDPBD)
            max_chunk = 0x4000u; /* 16 KiB */

        chunk = ((size - done) > max_chunk) ? max_chunk : (size - done);
        n = (int)read(fd, out + done, chunk);

        if (n <= 0 && dev == PS2_BOOT_MMCE && base_pos >= 0)
        {
            int attempt;

            /* MMCEMAN has bounded transfer timeouts. A timeout here is often
             * transient SIO2 contention with PADMAN, not EOF or corrupt data.
             * Re-seek to the exact expected byte and retry after yielding the
             * bus. */
            for (attempt = 1; attempt <= 3 && n <= 0; attempt++)
            {
                DelayThread(4000);
                if (lseek(fd, base_pos + (off_t)done, SEEK_SET) < 0)
                    break;

                ps2_log("MMCE: retrying asset read at 0x%08x (attempt %d)",
                        (unsigned)(base_pos + (off_t)done), attempt);
                n = (int)read(fd, out + done, chunk);
            }
        }

        if (n <= 0)
            return (done > 0) ? (int)done : n;

        done += (uint32_t)n;

        /* A returned MMCE read means MMCEMAN has unlocked SIO2. Leave a short
         * no-RPC window before the next chunk so PADMAN's IOP update thread
         * can actually acquire the bus. */
        if (dev == PS2_BOOT_MMCE && done < size)
            DelayThread(2000);
    }

    return (int)done;
}

int ps2_file_seek(int fd, uint32_t offset)
{
    return (int)lseek(fd, (off_t)offset, SEEK_SET);
}

int ps2_file_size(int fd)
{
    int size = (int)lseek(fd, 0, SEEK_END);

    lseek(fd, 0, SEEK_SET);
    return size;
}

void ps2_file_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}
