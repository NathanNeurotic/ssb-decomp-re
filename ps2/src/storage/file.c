/*
 * Minimal blocking file API used by the asset manager.
 *
 * The game sees one storage contract regardless of launch/data device:
 * open/read/seek/size/close. Device-specific details stay here.
 *
 * host: and cdrom0: use newlib/POSIX. Filesystems provided through iomanX
 * (mass/BDM, mc, PFS, UDPFS, MMCE, etc.) use fileXio directly so we do not
 * mix newlib descriptors/flags with native IOP descriptors.
 */
#include <ps2/platform.h>

#include <fileXio_rpc.h>
#include <fcntl.h>
#include <io_common.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PS2_FD_FILEXIO_TAG 0x40000000
#define PS2_FD_VALUE_MASK  0x3fffffff

static int uses_filexio_backend(void)
{
    switch (ps2_storage_data_device())
    {
    case PS2_BOOT_BDM:
    case PS2_BOOT_USB:
    case PS2_BOOT_MC:
    case PS2_BOOT_ATA:
    case PS2_BOOT_MX4SIO:
    case PS2_BOOT_ILINK:
    case PS2_BOOT_UDPBD:
    case PS2_BOOT_UDPFS:
    case PS2_BOOT_HDD:
    case PS2_BOOT_MMCE:
        return 1;
    case PS2_BOOT_HOST:
    case PS2_BOOT_CDROM:
    case PS2_BOOT_UNKNOWN:
    default:
        return 0;
    }
}

static int is_filexio_fd(int fd)
{
    return (fd & PS2_FD_FILEXIO_TAG) != 0;
}

static int raw_filexio_fd(int fd)
{
    return fd & PS2_FD_VALUE_MASK;
}

int ps2_file_open_read(const char *path)
{
    int fd;

    if (path == NULL)
        return -1;

    if (uses_filexio_backend())
    {
        /*
         * fileXio/iomanX uses FIO_* flags, not EE/newlib O_* flags.
         * This matters for MMCEMAN in particular: its open handler expects
         * FIO_O_RDONLY == 1. Passing newlib O_RDONLY (0) produces an invalid
         * packed MMCE open mode and can fail even though the path is valid.
         */
        fd = fileXioOpen(path, FIO_O_RDONLY, 0);
        if (fd < 0)
            return fd;
        return PS2_FD_FILEXIO_TAG | fd;
    }

    fd = open(path, O_RDONLY);

    /* ISO9660 paths on real hardware commonly require the ;1 version suffix,
     * while PCSX2 and some launchers accept the unversioned spelling. */
    if (fd < 0 && strncasecmp(path, "cdrom", 5) == 0)
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
     * MMCEMAN holds the SIO2 lock for the whole requested read. Its internal
     * DMA loop naturally batches at 16 * 256 = 4096 bytes. Keep each outer
     * request to one such batch so padman/memory-card traffic gets a chance
     * between chunks instead of being blocked behind a 64 KiB SIO2 transfer.
     */
    if (ps2_storage_data_device() == PS2_BOOT_MMCE)
        max_chunk = 0x1000u;

    while (done < size)
    {
        uint32_t chunk = ((size - done) > max_chunk) ? max_chunk : (size - done);
        int n = is_filexio_fd(fd)
                    ? fileXioRead(raw_filexio_fd(fd), out + done, (int)chunk)
                    : (int)read(fd, out + done, chunk);

        if (n <= 0)
            return (done > 0) ? (int)done : n;

        done += (uint32_t)n;
    }
    return (int)done;
}

int ps2_file_seek(int fd, uint32_t offset)
{
    if (is_filexio_fd(fd))
        return fileXioLseek(raw_filexio_fd(fd), (int)offset, SEEK_SET);

    return (int)lseek(fd, (off_t)offset, SEEK_SET);
}

int ps2_file_size(int fd)
{
    int size;

    if (is_filexio_fd(fd))
    {
        int raw = raw_filexio_fd(fd);

        size = fileXioLseek(raw, 0, SEEK_END);
        if (size >= 0)
            fileXioLseek(raw, 0, SEEK_SET);
        return size;
    }

    size = (int)lseek(fd, 0, SEEK_END);
    if (size >= 0)
        lseek(fd, 0, SEEK_SET);
    return size;
}

void ps2_file_close(int fd)
{
    if (fd < 0)
        return;

    if (is_filexio_fd(fd))
        fileXioClose(raw_filexio_fd(fd));
    else
        close(fd);
}
