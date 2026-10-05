/*
 * Minimal blocking file API used by the asset manager.
 *
 * Uses the ps2sdk newlib port's POSIX calls: the port routes each path to
 * the right IOP service (fileXio/iomanX for mass:, mmce:, pfs: and friends;
 * the BIOS ioman for host: and cdrom0:), so the asset code stays
 * device-agnostic. fileXio is initialised during IOP bring-up.
 */
#include <ps2/platform.h>

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

    /* Large reads are split so a single request never blocks too long. */
    while (done < size)
    {
        /* Keep individual RPCs modest. 64 KiB avoids long/fragile 256 KiB
         * transfers on real BDM/network stacks while remaining efficient. */
        uint32_t chunk = ((size - done) > 0x10000u) ? 0x10000u : (size - done);
        int n = (int)read(fd, out + done, chunk);

        if (n <= 0)
        {
            return (done > 0) ? (int)done : n;
        }
        done += (uint32_t)n;
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
