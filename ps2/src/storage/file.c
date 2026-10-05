/*
 * Minimal blocking file API used by the asset manager.
 *
 * Keep one device-agnostic storage contract. The ps2sdk newlib port routes
 * paths to the appropriate live IOP filesystem (mass:, mmce:, pfs:, udpfs:,
 * mc:, host:, cdrom0:, etc.), so the game never changes storage backend after
 * a file has opened.
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
    uint32_t max_chunk = 0x10000u;

    /*
     * MMCEMAN serializes access through SIO2 and holds that lock across the
     * entire requested read. Its transfer routine naturally works in batches
     * of at most 16 * 256 = 4096 bytes. Issue one batch per read() so padman
     * and memory-card traffic can run between chunks instead of waiting behind
     * the old 64 KiB request. Every other device keeps the proven 64 KiB cap.
     */
    if (ps2_storage_data_device() == PS2_BOOT_MMCE)
        max_chunk = 0x1000u;

    while (done < size)
    {
        uint32_t chunk = ((size - done) > max_chunk) ? max_chunk : (size - done);
        int n = (int)read(fd, out + done, chunk);

        if (n <= 0)
            return (done > 0) ? (int)done : n;

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

    if (size >= 0)
        lseek(fd, 0, SEEK_SET);
    return size;
}

void ps2_file_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
