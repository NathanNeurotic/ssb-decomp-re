/*
 * Minimal blocking file API used by the asset manager.
 *
 * Keep one device-agnostic storage contract. The ps2sdk newlib port routes
 * paths to the appropriate live IOP filesystem (mass:, mmce:, pfs:, udpfs:,
 * mc:, host:, cdrom0:, etc.), so the game never changes storage backend after
 * a file has opened.
 */
#include <ps2/platform.h>

#include <ps2sdkapi.h>
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
    uint32_t max_chunk = 0x4000u;

    /*
     * MMCEMAN serializes access through SIO2 and holds that lock across the
     * entire requested read. Its transfer routine naturally works in batches
     * of at most 16 * 256 = 4096 bytes. Issue one batch per read() so padman
     * and memory-card traffic can run between chunks instead of waiting behind
     * the old large request. Other physical/network backends use a 16 KiB
     * ceiling: small enough for conservative USB/BDM/network stacks without
     * penalizing host/CD/DVD-style access with tiny transactions.
     */
    if (ps2_storage_data_device() == PS2_BOOT_MMCE)
        max_chunk = 0x1000u;
    else if (ps2_storage_data_device() == PS2_BOOT_HOST ||
             ps2_storage_data_device() == PS2_BOOT_CDROM)
        max_chunk = 0x10000u;

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

int ps2_file_mmce_enter_runtime_stream(int fd)
{
    int remote_fd;
    int stream_fd;
    int port = 2;
    char path[48];
    const char *dir;

    if (ps2_storage_data_device() != PS2_BOOT_MMCE)
        return fd;

    /*
     * Crucial: obtain the MMCE card-side descriptor from the SAME newlib
     * handle that already loaded the pack header/table/resident data. The
     * earlier experiments reopened SSB64.DAT through a different API and
     * descriptor namespace; real hardware rejected that path.
     */
    remote_fd = _ps2sdk_ioctl2(fd, 0x80, NULL, 0, NULL, 0);
    if (remote_fd < 0)
    {
        ps2_log("MMCE: ioctl2(0x80) on live DAT fd failed (%d)", remote_fd);
        return -1;
    }

    dir = ps2_storage_boot_dir();
    if (dir != NULL && strncasecmp(dir, "mmce1:", 6) == 0)
        port = 3;

    ps2_log("MMCE: promoting live DAT fd to game stream (card fd=%d port=%d)",
            remote_fd, port);

    /*
     * This reset intentionally abandons the old EE/newlib descriptor without
     * closing it: closing would send FS_CLOSE and invalidate remote_fd. The
     * reset discards MMCEMAN/local fileXio state while the MMCE card keeps the
     * card-side descriptor alive for MMCEDRV.
     */
    if (ps2_iop_mmce_prepare_runtime_stream() < 0)
    {
        ps2_log("MMCE: final MMCEDRV IOP rebuild failed");
        return -1;
    }

    snprintf(path, sizeof(path), "ssbmmce:%d,%d", port, remote_fd);
    stream_fd = open(path, O_RDONLY);
    if (stream_fd < 0)
    {
        ps2_log("MMCE: in-game stream open failed (%d) for %s", stream_fd, path);
        return -1;
    }

    /* mmcedrv_get_size() used by stream_open leaves the card fd at EOF. */
    if (lseek(stream_fd, 0, SEEK_SET) < 0)
    {
        ps2_log("MMCE: in-game stream initial seek failed");
        close(stream_fd);
        return -1;
    }

    ps2_log("MMCE: SSB64.DAT now backed by MMCEDRV in-game stream");
    return stream_fd;
}

void ps2_file_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
