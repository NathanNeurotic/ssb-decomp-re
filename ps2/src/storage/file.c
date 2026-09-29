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
#include <unistd.h>

int ps2_file_open_read(const char *path)
{
    return open(path, O_RDONLY);
}

int ps2_file_read(int fd, void *dst, uint32_t size)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;

    /* Large reads are split so a single request never blocks too long. */
    while (done < size)
    {
        uint32_t chunk = ((size - done) > 0x40000u) ? 0x40000u : (size - done);
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
