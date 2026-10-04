/*
 * IOP bring-up and launch-device restoration.
 *
 * For every reconstructable device, reset to a clean IOP, install the common
 * service set, then restore only the transport that supplied the ELF. host:
 * and an already-mounted pfsN: are deliberately kept alive because resetting
 * them would destroy the filesystem represented by argv[0].
 */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <ctype.h>
#include <fcntl.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DECLARE_IRX(name) \
    extern unsigned char name##_irx[]; \
    extern unsigned int size_##name##_irx

DECLARE_IRX(iomanx);
DECLARE_IRX(filexio);
DECLARE_IRX(sio2man);
DECLARE_IRX(mtapman);
DECLARE_IRX(padman);
DECLARE_IRX(mcman);
DECLARE_IRX(mcserv);
DECLARE_IRX(libsd);
DECLARE_IRX(sdr);

DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mx4sio_bd);
DECLARE_IRX(iLinkman);
DECLARE_IRX(IEEE1394_bd);
DECLARE_IRX(mmceman);

DECLARE_IRX(ps2dev9);
DECLARE_IRX(ps2atad);
DECLARE_IRX(ps2hdd);
DECLARE_IRX(ps2fs);

DECLARE_IRX(smap);
DECLARE_IRX(ministack);
DECLARE_IRX(udpbd);
DECLARE_IRX(udpfs_ioman);

#define MAX_TRACKED_MODULES 32

static const char *sLoaded[MAX_TRACKED_MODULES];
static int sLoadedCount;
static int sIopWasReset;

static int module_loaded(const char *name)
{
    int i;

    for (i = 0; i < sLoadedCount; i++)
    {
        if (strcmp(sLoaded[i], name) == 0)
            return 1;
    }
    return 0;
}

static int load_irx(const char *name, void *buf, unsigned int size,
                    const char *args, int args_len)
{
    int result = 0;
    int id;

    if (module_loaded(name))
        return 0;

    id = SifExecModuleBuffer(buf, size, (u32)args_len, args, &result);
    if (id < 0 || result == 1)
    {
        ps2_log("IOP: %s failed (id=%d res=%d)", name, id, result);
        return -1;
    }

    if (sLoadedCount < MAX_TRACKED_MODULES)
        sLoaded[sLoadedCount++] = name;
    ps2_log("IOP: loaded %s (%u bytes)", name, size);
    return 0;
}

#define LOAD_IRX(name) load_irx(#name, name##_irx, size_##name##_irx, NULL, 0)
#define LOAD_IRX_AS(label, name) load_irx(label, name##_irx, size_##name##_irx, NULL, 0)

static int load_bdm_core(void)
{
    if (LOAD_IRX(bdm) < 0)
        return -1;
    if (LOAD_IRX(bdmfs_fatfs) < 0)
        return -1;
    return 0;
}

static int load_dev9(void)
{
    return LOAD_IRX(ps2dev9);
}

static int load_usb(void)
{
    if (load_bdm_core() < 0)
        return -1;
    if (LOAD_IRX(usbd_mini) < 0)
        return -1;
    return LOAD_IRX(usbmass_bd_mini);
}

static int load_mx4sio(void)
{
    if (load_bdm_core() < 0)
        return -1;
    return LOAD_IRX_AS("mx4sio_bd", mx4sio_bd);
}

static int load_ilink(void)
{
    if (load_bdm_core() < 0)
        return -1;
    if (LOAD_IRX(iLinkman) < 0)
        return -1;
    return LOAD_IRX_AS("IEEE1394_bd", IEEE1394_bd);
}

static int load_ata(void)
{
    if (load_dev9() < 0)
        return -1;
    if (load_bdm_core() < 0)
        return -1;
    if (LOAD_IRX_AS("ps2atad", ps2atad) < 0)
        return -1;

    /* Physical HDDs need a short post-ATAD settle period before probing. */
    sleep(1);
    return 0;
}

static int read_ip_argument(char *out, size_t out_size)
{
    static const char *paths[] = {
        "mc0:/SYS-CONF/IPCONFIG.DAT",
        "mc1:/SYS-CONF/IPCONFIG.DAT",
    };
    char buf[96];
    int p;

    for (p = 0; p < 2; p++)
    {
        int fd = open(paths[p], O_RDONLY);
        int n;
        int i;

        if (fd < 0)
            continue;

        n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;

        buf[n] = '\0';
        i = 0;
        while (buf[i] != '\0' &&
               !isspace((unsigned char)buf[i]) &&
               buf[i] != ',')
            i++;
        buf[i] = '\0';

        if (strchr(buf, '.') == NULL)
            continue;

        snprintf(out, out_size, "ip=%s", buf);
        ps2_log("IOP: network IP from %s", paths[p]);
        return 0;
    }

    ps2_log("IOP: network boot needs mc?:/SYS-CONF/IPCONFIG.DAT");
    return -1;
}

static int load_network_base(void)
{
    char iparg[32];

    if (load_dev9() < 0)
        return -1;
    if (LOAD_IRX(smap) < 0)
        return -1;
    if (read_ip_argument(iparg, sizeof(iparg)) < 0)
        return -1;
    if (load_irx("ministack", ministack_irx, size_ministack_irx,
                 iparg, (int)strlen(iparg) + 1) < 0)
        return -1;
    return 0;
}

static int load_udpbd(void)
{
    if (load_bdm_core() < 0)
        return -1;
    if (load_network_base() < 0)
        return -1;
    return LOAD_IRX(udpbd);
}

static int load_udpfs(void)
{
    if (load_network_base() < 0)
        return -1;
    return LOAD_IRX(udpfs_ioman);
}

static int wait_for_resolution(int timeout_ms)
{
    int elapsed;

    for (elapsed = 0; elapsed <= timeout_ms; elapsed += 100)
    {
        if (ps2_storage_resolve_boot_path())
            return 0;
        DelayThread(100 * 1000);
    }
    return -1;
}

static int load_apa_and_mount(void)
{
    static const char hdd_args[] = "-o\0" "4\0" "-n\0" "20";
    static const char pfs_args[] = "-o\0" "10\0" "-n\0" "40";
    const char *partition = ps2_storage_hdd_partition();
    char hdd_root[8] = "hdd0:";
    int fd;
    int i;

    if (partition == NULL || partition[0] == '\0')
    {
        /* Direct pfsN: launch: the caller's existing mount is the only
         * partition identity available, and ps2_iop_init preserved it. */
        return 0;
    }

    if (load_ata() < 0)
        return -1;
    if (load_irx("ps2hdd", ps2hdd_irx, size_ps2hdd_irx,
                 hdd_args, sizeof(hdd_args)) < 0)
        return -1;
    if (load_irx("ps2fs", ps2fs_irx, size_ps2fs_irx,
                 pfs_args, sizeof(pfs_args)) < 0)
        return -1;

    if (!strncmp(partition, "hdd1:", 5))
        snprintf(hdd_root, sizeof(hdd_root), "hdd1:");

    for (i = 0; i < 100; i++)
    {
        fd = open(hdd_root, O_RDONLY | O_DIRECTORY);
        if (fd >= 0)
        {
            close(fd);
            break;
        }
        DelayThread(100 * 1000);
    }
    if (i == 100)
    {
        ps2_log("IOP: %s did not become ready", hdd_root);
        return -1;
    }

    /* Assets are read-only and game saves live on memory card. */
    if (fileXioMount("pfs0:", partition, FIO_MT_RDONLY) != 0)
    {
        ps2_log("IOP: failed to mount %s on pfs0:", partition);
        return -1;
    }
    ps2_log("IOP: mounted %s on pfs0:", partition);
    return 0;
}

void ps2_iop_init(void)
{
    sLoadedCount = 0;
    sIopWasReset = 0;

    SifInitRpc(0);

    if (ps2_storage_should_reset_iop())
    {
        while (!SifIopReset("", 0))
        {
        }
        while (!SifIopSync())
        {
        }
        SifInitRpc(0);
        sIopWasReset = 1;
    }

    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    LOAD_IRX(iomanx);
    if (LOAD_IRX(filexio) == 0)
        fileXioInit();
    else if (!sIopWasReset)
        fileXioInit();

    LOAD_IRX(sio2man);
    LOAD_IRX(mtapman);
    LOAD_IRX(padman);
    LOAD_IRX(mcman);
    LOAD_IRX(mcserv);
    LOAD_IRX(libsd);
    LOAD_IRX(sdr);

    ps2_log("IOP: %s, %d modules installed by SSB64",
            sIopWasReset ? "reset" : "preserved", sLoadedCount);
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    int ret;

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_CDROM:
        return 0;

    case PS2_BOOT_MC:
        return wait_for_resolution(3000);

    case PS2_BOOT_MMCE:
        if (LOAD_IRX(mmceman) < 0)
            return -1;
        return wait_for_resolution(10000);

    case PS2_BOOT_USB:
        if (load_usb() < 0)
            return -1;
        return wait_for_resolution(12000);

    case PS2_BOOT_MX4SIO:
        if (load_mx4sio() < 0)
            return -1;
        return wait_for_resolution(12000);

    case PS2_BOOT_ILINK:
        if (load_ilink() < 0)
            return -1;
        return wait_for_resolution(12000);

    case PS2_BOOT_ATA:
        if (load_ata() < 0)
            return -1;
        return wait_for_resolution(12000);

    case PS2_BOOT_UDPBD:
        if (load_udpbd() < 0)
            return -1;
        return wait_for_resolution(12000);

    case PS2_BOOT_UDPFS:
        return load_udpfs();

    case PS2_BOOT_HDD:
        return load_apa_and_mount();

    case PS2_BOOT_BDM:
        /*
         * massN: identifies a filesystem slot, not its transport. Restore
         * transports in increasing hardware cost and stop as soon as the
         * literal slot (or a bare mass: boot ELF) reappears. In particular,
         * do not load MX4SIO on a normal USB boot: it shares SIO2 with pads.
         */
        if (load_usb() == 0 && wait_for_resolution(3000) == 0)
            return 0;
        if (load_mx4sio() == 0 && wait_for_resolution(3000) == 0)
            return 0;

        ret = load_ilink();
        if (load_ata() < 0 && ret < 0)
            ps2_log("IOP: neither iLink nor ATA transport initialized");
        if (wait_for_resolution(8000) == 0)
            return 0;

        /* A launcher can also hand UDPBD to us as an indistinguishable massN:. */
        if (load_udpbd() == 0 && wait_for_resolution(5000) == 0)
            return 0;
        return -1;

    case PS2_BOOT_UNKNOWN:
    default:
        ps2_log("IOP: unsupported launch path: %s", ps2_storage_original_path());
        return -1;
    }
}

int ps2_iop_module_loaded(const char *name)
{
    return module_loaded(name);
}

int ps2_iop_module_count(void)
{
    return sLoadedCount;
}

const char *ps2_iop_module_name(int i)
{
    return (i >= 0 && i < sLoadedCount) ? sLoaded[i] : "";
}
