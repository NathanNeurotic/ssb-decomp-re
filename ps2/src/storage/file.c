/*
 * Minimal blocking file API used by the asset manager.
 *
 * Uses the ps2sdk newlib port's POSIX calls: the port routes each path to
 * the right IOP service (fileXio/iomanX for mass:, mmce:, pfs: and friends;
 * the BIOS ioman for host: and cdrom0:), so the asset code stays
 * device-agnostic. fileXio is initialised during IOP bring-up.
 */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <fileXio_rpc.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PS2_FD_MMCE_STREAM_TAG 0x40000000
#define PS2_FD_VALUE_MASK      0x3fffffff
#define MMCE_IOCTL_GET_FD      0x80

static char sMmceDatPath[384];
static int sMmceSetupNativeFd = -1;

static int is_mmce_stream_fd(int fd)
{
    return (fd & PS2_FD_MMCE_STREAM_TAG) != 0;
}

static int raw_stream_fd(int fd)
{
    return fd & PS2_FD_VALUE_MASK;
}

int ps2_file_open_read(const char *path)
{
    int fd;

    /* Keep the proven POSIX/newlib MMCE open path for setup. Only remember the
     * path here; the native fileXio descriptor used for MMCEDRV handoff is
     * opened later, after SSB64.DAT has already been validated. */
    if (path != NULL && ps2_storage_data_device() == PS2_BOOT_MMCE &&
        strncasecmp(path, "mmce", 4) == 0)
    {
        strncpy(sMmceDatPath, path, sizeof(sMmceDatPath) - 1);
        sMmceDatPath[sizeof(sMmceDatPath) - 1] = '\0';
    }

    fd = open(path, O_RDONLY);

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
        int n = is_mmce_stream_fd(fd)
                    ? fileXioRead(raw_stream_fd(fd), out + done, (int)chunk)
                    : (int)read(fd, out + done, chunk);

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
    if (is_mmce_stream_fd(fd))
        return fileXioLseek(raw_stream_fd(fd), (int)offset, SEEK_SET);

    return (int)lseek(fd, (off_t)offset, SEEK_SET);
}

int ps2_file_size(int fd)
{
    int size;

    if (is_mmce_stream_fd(fd))
    {
        int rawfd = raw_stream_fd(fd);
        size = fileXioLseek(rawfd, 0, SEEK_END);
        fileXioLseek(rawfd, 0, SEEK_SET);
        return size;
    }

    size = (int)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return size;
}

int ps2_file_mmce_enter_runtime_stream(int fd)
{
    int remote_fd;
    int stream_fd;
    int port = 2;
    char stream_path[48];
    const char *boot_dir;

    if (ps2_storage_data_device() != PS2_BOOT_MMCE)
        return fd;

    if (sMmceDatPath[0] == '\0')
    {
        ps2_log("MMCE: no DAT path remembered for runtime handoff");
        return -1;
    }

    /* First half of the RiptOPL/wOPL handoff: open with MMCEMAN and ask
     * ioctl2 0x80 for the MMCE-side descriptor. The next step deliberately
     * crosses a clean IOP reset; the card-side descriptor survives on the
     * MMCE device and is then consumed by MMCEDRV in the runtime IOP. */
    sMmceSetupNativeFd = fileXioOpen(sMmceDatPath, O_RDONLY, 0);
    if (sMmceSetupNativeFd < 0)
    {
        ps2_log("MMCE: native setup open failed (%d)", sMmceSetupNativeFd);
        return -1;
    }

    remote_fd = fileXioIoctl2(sMmceSetupNativeFd, MMCE_IOCTL_GET_FD,
                              NULL, 0, NULL, 0);
    if (remote_fd < 0)
    {
        ps2_log("MMCE: ioctl2 GET_FD failed (%d)", remote_fd);
        return -1;
    }

    boot_dir = ps2_storage_boot_dir();
    if (boot_dir != NULL && strncasecmp(boot_dir, "mmce1:", 6) == 0)
        port = 3;

    ps2_log("MMCE: DAT native fd=%d device fd=%d port=%d",
            sMmceSetupNativeFd, remote_fd, port);

    if (ps2_iop_mmce_prepare_runtime_stream() < 0)
    {
        ps2_log("MMCE: failed to load MMCEDRV runtime stack");
        return -1;
    }

    snprintf(stream_path, sizeof(stream_path), "ssbmmce:%d,%d", port, remote_fd);
    stream_fd = fileXioOpen(stream_path, O_RDONLY, 0);
    if (stream_fd < 0)
    {
        ps2_log("MMCE: runtime bridge open failed (%d)", stream_fd);
        return -1;
    }

    if (ps2_iop_mmce_finish_runtime_services() < 0)
    {
        ps2_log("MMCE: DAT fd valid, but post-MMCE pad/mc services failed");
        fileXioClose(stream_fd);
        return -1;
    }

    ps2_log("MMCE: runtime DAT reads switched to MMCEDRV");
    return PS2_FD_MMCE_STREAM_TAG | stream_fd;
}

void ps2_file_close(int fd)
{
    if (fd < 0)
        return;

    if (is_mmce_stream_fd(fd))
        fileXioClose(raw_stream_fd(fd));
    else
        close(fd);
}
