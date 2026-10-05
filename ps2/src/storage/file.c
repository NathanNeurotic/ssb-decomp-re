/*
 * Minimal blocking file API used by the asset manager.
 *
 * Uses the ps2sdk newlib port's POSIX calls. The IOP handoff selects the
 * inherited filesystem RPC first: legacy FileIO/ioman for host-like paths, or
 * fileXio/iomanX for BDM/PFS/MMCE paths. fileXioInit() switches libcglue to
 * its iomanX-backed operations without replacing the launcher's device
 * drivers or mounts.
 */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <delaythread.h>
#include <fcntl.h>
#include <fileXio_rpc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PS2_FD_MMCE_STREAM_TAG 0x20000000
#define PS2_FD_TAG_MASK         0x60000000
#define PS2_FD_VALUE_MASK       0x1fffffff
#define MMCE_IOCTL_GET_FD       0x80

static char sMmceSetupPath[384];

static int tagged_filexio_fd(int fd)
{
    return (fd & PS2_FD_TAG_MASK) == PS2_FD_MMCE_STREAM_TAG;
}

static int raw_filexio_fd(int fd)
{
    return fd & PS2_FD_VALUE_MASK;
}

int ps2_file_open_read(const char *path)
{
    int fd;

    /* Boot-time MMCE access must stay on the exact POSIX/newlib path that is
     * already proven on hardware. Remember the DAT pathname so the later
     * MMCEDRV handoff can open a second native fileXio descriptor only after
     * the pack has been validated. */
    if (ps2_storage_data_device() == PS2_BOOT_MMCE &&
        path != NULL && strncasecmp(path, "mmce", 4) == 0)
    {
        strncpy(sMmceSetupPath, path, sizeof(sMmceSetupPath) - 1);
        sMmceSetupPath[sizeof(sMmceSetupPath) - 1] = '\0';
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
    PS2BootDevice dev = ps2_storage_data_device();
    off_t base_pos = -1;
    int use_filexio = tagged_filexio_fd(fd);
    int rawfd = raw_filexio_fd(fd);

    /* MMCE setup and gameplay descriptors are intentionally native fileXio
     * descriptors. Capture the current position using that same client. */
    if (dev == PS2_BOOT_MMCE)
        base_pos = use_filexio ? (off_t)fileXioLseek(rawfd, 0, SEEK_CUR)
                               : lseek(fd, 0, SEEK_CUR);

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
        n = use_filexio ? fileXioRead(rawfd, out + done, (int)chunk)
                        : (int)read(fd, out + done, chunk);

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
                if ((use_filexio
                         ? fileXioLseek(rawfd, (int)(base_pos + (off_t)done), SEEK_SET)
                         : (int)lseek(fd, base_pos + (off_t)done, SEEK_SET)) < 0)
                    break;

                ps2_log("MMCE: retrying asset read at 0x%08x (attempt %d)",
                        (unsigned)(base_pos + (off_t)done), attempt);
                n = use_filexio ? fileXioRead(rawfd, out + done, (int)chunk)
                                : (int)read(fd, out + done, chunk);
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
    if (tagged_filexio_fd(fd))
        return fileXioLseek(raw_filexio_fd(fd), (int)offset, SEEK_SET);

    return (int)lseek(fd, (off_t)offset, SEEK_SET);
}

int ps2_file_size(int fd)
{
    int size;

    if (tagged_filexio_fd(fd))
    {
        int rawfd = raw_filexio_fd(fd);
        size = fileXioLseek(rawfd, 0, SEEK_END);
        fileXioLseek(rawfd, 0, SEEK_SET);
        return size;
    }

    size = (int)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return size;
}

int ps2_file_mmce_enter_streaming(int fd)
{
    int setup_native_fd;
    int remote_fd;
    int port = 2;
    int stream_fd;
    char stream_path[48];
    const char *boot_dir;

    if (ps2_storage_data_device() != PS2_BOOT_MMCE)
        return fd;

    if (sMmceSetupPath[0] == '\0')
    {
        ps2_log("MMCE: no remembered DAT path for streaming handoff");
        return -1;
    }

    /* Do not disturb the working POSIX descriptor used during boot. Initialise
     * the native fileXio client here, then open a second MMCEMAN descriptor
     * solely to obtain the card-side fd required by MMCEDRV. */
    if (fileXioInit() < 0)
    {
        ps2_log("MMCE: native fileXio client init failed");
        return -1;
    }

    setup_native_fd = fileXioOpen(sMmceSetupPath, O_RDONLY, 0);
    if (setup_native_fd < 0)
    {
        ps2_log("MMCE: native handoff open failed for %s", sMmceSetupPath);
        return -1;
    }

    /* MMCEMAN exposes the MMCE device's own descriptor through ioctl 0x80.
     * That descriptor remains meaningful to MMCEDRV after MMCEMAN leaves. */
    {
        int dummy = 0;

        /* MMCEMAN implements MMCE_CMD_IOCTL_GET_FD in its ioctl2 handler,
         * not ioctl. The previous handoff used fileXioIoctl(), so the DAT was
         * successfully opened and validated and then we asked the wrong IOP
         * operation for its card-side descriptor. */
        remote_fd = fileXioIoctl2(setup_native_fd, MMCE_IOCTL_GET_FD,
                                  &dummy, 0, &dummy, 0);
    }
    if (remote_fd < 0)
    {
        ps2_log("MMCE: ioctl2 GET_FD failed (%d)", remote_fd);
        return -1;
    }

    boot_dir = ps2_storage_boot_dir();
    if (boot_dir != NULL && strncasecmp(boot_dir, "mmce1:", 6) == 0)
        port = 3;

    ps2_log("MMCE: DAT POSIX fd=%d native fd=%d card fd=%d port=%d",
            fd, setup_native_fd, remote_fd, port);

    /* Deliberately do not close either MMCEMAN descriptor before unloading
     * MMCEMAN: closing the native one would also close the card-side fd that
     * is being handed to MMCEDRV. */
    if (ps2_iop_mmce_enter_streaming() < 0)
        return -1;

    snprintf(stream_path, sizeof(stream_path), "ssbmmce:%d,%d", port, remote_fd);
    stream_fd = fileXioOpen(stream_path, O_RDONLY, 0);
    if (stream_fd < 0)
    {
        ps2_log("MMCE: could not open MMCEDRV streaming bridge");
        return -1;
    }

    ps2_log("MMCE: runtime DAT reads now use MMCEDRV");
    return PS2_FD_MMCE_STREAM_TAG | stream_fd;
}

void ps2_file_close(int fd)
{
    if (fd < 0)
        return;

    if (tagged_filexio_fd(fd))
        fileXioClose(raw_filexio_fd(fd));
    else
        close(fd);
}
