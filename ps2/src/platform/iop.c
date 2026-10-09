/*
 * IOP bring-up for real launch/data devices.
 *
 * The launch device and the data device are intentionally separate.  host:
 * keeps the ps2link/PCSX2 IOP alive; every other launch starts from a clean
 * IOP and reconstructs only the stack required by the selected data device.
 *
 * Base:      iomanX + fileXio + sio2man + mtapman + padman + mcman/mcserv
 *            + libsd/sdr
 * USB:       bdm + bdmfs_fatfs + usbd_mini + usbmass_bd_mini
 * ATA BDM:   ps2dev9 + bdm + bdmfs_fatfs + ps2atad
 * MX4SIO:    bdm + bdmfs_fatfs + mx4sio_bd
 * iLink:     bdm + bdmfs_fatfs + iLinkman + IEEE1394_bd
 * UDPBD:     ps2dev9 + bdm + bdmfs_fatfs + smap_udpbd(ip=...)
 * UDPFS:     ps2dev9 + udpfs_smap + udpfs_ministack(ip=...) + udpfs_ioman
 * APA/PFS:   ps2dev9 + bdm + bdmfs_fatfs + ps2atad + ps2hdd + ps2fs
 * MMCE:      mmceman
 */
#define NEWLIB_PORT_AWARE
#include <ps2/platform.h>

#include <ctype.h>
#include <delaythread.h>
#include <fcntl.h>
#include <fileio.h>
#include <fileXio_rpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <io_common.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <smod.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DECLARE_IRX(name)                 \
    extern unsigned char name##_irx[];    \
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
DECLARE_IRX(ssb_audio);

DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mmceman);
DECLARE_IRX(mmcedrv);
DECLARE_IRX(ssb_mmce_stream);
DECLARE_IRX(cdvd);

DECLARE_IRX(ps2dev9);
DECLARE_IRX(ps2atad);
DECLARE_IRX(mx4sio_bd);
DECLARE_IRX(iLinkman);
DECLARE_IRX(IEEE1394_bd);
DECLARE_IRX(smap_udpbd);
DECLARE_IRX(udpfs_smap);
DECLARE_IRX(udpfs_ministack);
DECLARE_IRX(udpfs_ioman);
DECLARE_IRX(ps2hdd);
DECLARE_IRX(ps2fs);
DECLARE_IRX(secrsif);

#define MAX_TRACKED_MODULES 32

static const char *sLoaded[MAX_TRACKED_MODULES];
static int sLoadedCount;
static int sIopWasReset;
static int sInheritedBdm;
/* Diagnostics for the particular IRX whose load may legitimately fail when
 * an existing launcher owns the RPC service. */
static int sLastFileXioModuleId = -999;
static int sLastFileXioModuleResult = -999;

static int load_irx(const char *name, void *buf, unsigned int size, const char *args, int args_len)
{
    int result = 0;
    int id;

    if (sInheritedBdm)
    {
        /* Keep RPC clients and SIO2 hooks bound to the launcher's services. */
        static const struct { const char *irx; const char *module; } existing[] = {
            { "filexio", "IOX/File_Manager_Rpc" },
            { "sio2man", "sio2man" },
            { "mtapman", "multitap_manager" },
            { "padman", "padman" },
            { "mcman", "mcman_cex" },
            { "mcman", "mcman" },
            { "mcserv", "mcserv" },
            { "libsd", "freesd" },
            { "libsd", "libsd" },
        };
        smod_mod_info_t info;
        unsigned int i;

        for (i = 0; i < sizeof(existing) / sizeof(existing[0]); i++)
        {
            if (strcmp(name, existing[i].irx) == 0 &&
                smod_get_mod_by_name(existing[i].module, &info))
            {
                if (sLoadedCount < MAX_TRACKED_MODULES)
                    sLoaded[sLoadedCount++] = name;
                ps2_log("IOP: reusing %s", name);
                return 0;
            }
        }
    }
    id = SifExecModuleBuffer(buf, size, (u32)args_len, args, &result);
    if (strcmp(name, "filexio") == 0)
    {
        sLastFileXioModuleId = id;
        sLastFileXioModuleResult = result;
    }

    if (id < 0 || result == 1 /* NO_RESIDENT_END */)
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
#define LOAD_IRX_ARGS(name, args, len) load_irx(#name, name##_irx, size_##name##_irx, args, len)

/* The launcher may supply fileXio even if the module name differs. Binding
 * with mode=0 is synchronous; it may transiently return -E_SIF_PKT_ALLOC
 * or -E_SIF_PKT_SEND while IOP/SIF is still settling. The earlier audit
 * treated the first negative result as "no server", tried to load a duplicate
 * module and immediately aborted when that duplicate was rejected. */
static int inherited_filexio_rpc_ready(void)
{
    static SifRpcClientData_t probe __attribute__((aligned(64)));
    int i;

    for (i = 0; i < 100; i++)
    {
        int rc;

        memset(&probe, 0, sizeof(probe));
        rc = sceSifBindRpc(&probe, FILEXIO_IRX, 0);
        if (rc >= 0 && probe.server != NULL)
            return 1;
        /* A negative result here is a failed bind attempt, NOT evidence
         * that the RPC service does not exist. Retry before declaring loss. */
        DelayThread(10000);
    }
    return 0;
}

/* Inherited launchers may expose the standard FILEIO service without a
 * modern iomanX export table. The embedded filexio.irx imports dozens of
 * iomanX exports, so its load can fail with -E_IOP_DEPENDANCY (-200), even
 * though the launcher's mass mount works through FILEIO and legacy ioman.
 *
 * PS2SDK libcglue defaults to fio-backed POSIX open/read/lseek/close, and
 * fileXioInit() changes that global backend. If a usable inherited FILEIO
 * server exists, keep the default backend and don't reset the IOP.
 */
static int inherited_fileio_fallback(void)
{
    static SifRpcClientData_t probe __attribute__((aligned(64)));
    int attempt;

    if (!sInheritedBdm)
        return 0;

    for (attempt = 0; attempt < 50; ++attempt)
    {
        int ret;

        memset(&probe, 0, sizeof(probe));
        ret = sceSifBindRpc(&probe, 0x80000001, 0); /* PS2SDK FILEIO RPC ID */
        if (ret >= 0 && probe.server != NULL)
        {
            if (fioInit() >= 0)
            {
                ps2_log("IOP: inherited FILEIO fallback active; preserving mass mount");
                return 1;
            }
            return 0;
        }
        DelayThread(10000);
    }

    return 0;
}

/* Return 0 for fileXio, 1 for inherited FILEIO, and <0 only when neither
 * path is operational. Success of a module load alone is insufficient. */
static int init_filexio_runtime(void)
{
    int ready = sInheritedBdm && inherited_filexio_rpc_ready();
    int load_result = 0;

    if (!ready)
    {
        load_result = LOAD_IRX(filexio);
        if (load_result < 0)
            ps2_log("IOP: fileXio module load rejected; checking RPC before failing");
        ready = inherited_filexio_rpc_ready();
    }

    if (!ready)
    {
        /* A fileXio import dependency failure need not destroy a live mass
         * filesystem. The existing FILEIO service can still stream DATs. */
        if (inherited_fileio_fallback())
        {
            ps2_log("IOP: no fileXio, using inherited FILEIO (id=%d result=%d)",
                    sLastFileXioModuleId, sLastFileXioModuleResult);
            return 1;
        }

        ps2_log("IOP: neither fileXio nor FILEIO available (inherited=%d load=%d id=%d result=%d)",
                sInheritedBdm, load_result, sLastFileXioModuleId, sLastFileXioModuleResult);
        return -1;
    }

    /* fileXioInit binds PS2SDK's real static client and registers newlib's
     * file/device hooks. Our temporary probe never substitutes for it. */
    if (fileXioInit() < 0)
        return -2;

    return 0;
}

static int load_bdm_core(void)
{
    if (LOAD_IRX(bdm) < 0)
        return -1;
    if (LOAD_IRX(bdmfs_fatfs) < 0)
        return -1;
    return 0;
}

static int is_ipv4_token(const char *s)
{
    int octets = 0;
    int digits = 0;
    int value = 0;

    for (; *s != '\0'; s++)
    {
        if (*s >= '0' && *s <= '9')
        {
            if (++digits > 3)
                return 0;
            value = value * 10 + (*s - '0');
            if (value > 255)
                return 0;
        }
        else if (*s == '.' && digits != 0 && octets < 3)
        {
            octets++;
            digits = 0;
            value = 0;
        }
        else
        {
            return 0;
        }
    }

    return octets == 3 && digits != 0;
}

static int read_ip_arg(char *out, size_t out_size)
{
    static const char *paths[] = {
        "mc0:/SYS-CONF/IPCONFIG.DAT",
        "mc1:/SYS-CONF/IPCONFIG.DAT",
    };
    char buf[128];
    int i;

    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        int fd = open(paths[i], O_RDONLY);
        int n;
        char *p, *end;

        if (fd < 0)
            continue;
        n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';

        p = buf;
        while (*p != '\0' && isspace((unsigned char)*p))
            p++;
        end = p;
        while (*end != '\0' && !isspace((unsigned char)*end))
            end++;
        *end = '\0';

        if (*p != '\0' && strlen(p) <= 15 && is_ipv4_token(p))
        {
            snprintf(out, out_size, "ip=%s", p);
            ps2_log("IOP: network IP from %s: %s", paths[i], p);
            return 1;
        }
    }

    ps2_log("IOP: network device needs mc?:/SYS-CONF/IPCONFIG.DAT");
    return 0;
}

static int mount_hdd_partition(void)
{
    const char *source = ps2_storage_hdd_mount_source();
    int attempt;
    int r = -1;

    if (source == NULL || source[0] == '\0')
    {
        ps2_log("IOP: bare pfs: path has no APA partition to remount");
        return -1;
    }

    for (attempt = 0; attempt < 40; attempt++)
    {
        r = fileXioMount("pfs0:", source, FIO_MT_RDWR);
        if (r >= 0)
        {
            ps2_log("IOP: mounted %s on pfs0:", source);
            return 0;
        }
        DelayThread(250 * 1000);
    }

    ps2_log("IOP: failed to mount %s on pfs0: (%d)", source, r);
    return -1;
}

void ps2_iop_init(void)
{
    int preserve_iop = ps2_storage_requires_iop_preserve();
    PS2BootDevice device = ps2_storage_data_device();

    sInheritedBdm = preserve_iop &&
        (device == PS2_BOOT_BDM || device == PS2_BOOT_USB ||
         device == PS2_BOOT_MX4SIO || device == PS2_BOOT_ATA ||
         device == PS2_BOOT_ILINK || device == PS2_BOOT_UDPBD);

    sLoadedCount = 0;
    sIopWasReset = 0;

    ps2_boot_stage("IOP: RPC initialization", 0xFFFF00);
    SifInitRpc(0);

    /* host: and bare pfsN: data paths depend on services/mounts owned by the
     * launcher.  Everything else is rebuilt from a known IOP state. */
    if (!preserve_iop)
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

    ps2_boot_stage("IOP: loadfile and heap initialization", 0xFFFFFF);
    SifLoadFileInit();
    SifInitIopHeap();

    ps2_boot_stage("IOP: module loader patches", 0xFF8000);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    if (sIopWasReset)
        sbv_patch_fileio();

    ps2_boot_stage("IOP: filesystem service initialization", 0x0000FF);

    /* A mass mount belongs to the launcher's I/O manager. Installing
     * another iomanX can bind our fileXio to a new, empty device table while
     * the actual mass driver stays registered with the original manager. */
    if (sInheritedBdm)
        ps2_log("IOP: preserving mounted BDM I/O manager");
    else if (LOAD_IRX(iomanx) < 0)
        ps2_panic("iomanX initialization failed");

    /* The server is the dependency, not successful duplicate module loading.
     * Probe, optionally load, then probe again before PS2SDK fileXioInit. */
    {
        int filexio_state = init_filexio_runtime();

        if (filexio_state == -1)
            ps2_panic("no fileXio/FILEIO RPC: inherited=%d module id=%d result=%d",
                      sInheritedBdm, sLastFileXioModuleId, sLastFileXioModuleResult);
        if (filexio_state < 0)
            ps2_panic("fileXio EE client initialization failed (%d)", filexio_state);
        if (filexio_state == 1)
            ps2_log("IOP: using launcher FILEIO backend for game data");
    }
    ps2_boot_stage("IOP: fileXio binding", 0x00FFFF);

    ps2_boot_stage("IOP: SIO2 and controller modules", 0x00FF00);
    /* Controller services are mandatory: a silent load failure leaves the
     * game running with no input and makes later recovery impossible. */
    if (LOAD_IRX(sio2man) < 0 || LOAD_IRX(mtapman) < 0 || LOAD_IRX(padman) < 0)
        ps2_panic("IOP controller stack initialization failed");
    /* Saving and SPU2 are degradable: allow gameplay if either is missing. */
    if (LOAD_IRX(mcman) < 0 || LOAD_IRX(mcserv) < 0)
        ps2_log("IOP: memory-card services unavailable; saves may be disabled");
    ps2_boot_stage("IOP: sound library", 0xFF0000);
    if (LOAD_IRX(libsd) < 0)
        ps2_log("IOP: libsd unavailable; audio may be disabled");
    /*
     * Real hardware has already proven the embedded sdr server can wedge this
     * port during startup. Storage work must not be masked by an unrelated
     * audio RPC hang, so keep SDR deferred until the common storage path is
     * stable on hardware.
     */
    ps2_log("IOP: sdr deferred for hardware-safe storage validation");

    ps2_log("IOP: %s, %d base modules",
            sIopWasReset ? "reset" : "kept (inherited filesystem)", sLoadedCount);
}

int ps2_iop_mmce_prepare_runtime_stream(void)
{
    if (ps2_storage_data_device() != PS2_BOOT_MMCE)
        return -1;

    /*
     * MMCEMAN is the setup/filesystem driver. MMCEDRV is explicitly the MMCE
     * project's in-game streaming driver. Preserve the card-side DAT handle
     * across one deliberate reset, then rebuild the final IOP before the EE
     * controller client is initialized.
     *
     * Loading MMCEDRV beside MMCEMAN is not safe: both install SIO2MAN hooks.
     * The reset is what discards MMCEMAN's hook without issuing FS_CLOSE to
     * the card-side descriptor.
     */
    ps2_log("IOP: MMCE setup complete; rebuilding final in-game IOP");

    fileXioExit();
    SifExitIopHeap();
    SifLoadFileExit();

    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}

    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    sLoadedCount = 0;
    sIopWasReset = 1;

    if (LOAD_IRX(iomanx) < 0 || LOAD_IRX(filexio) < 0)
        return -1;
    if (fileXioInit() < 0)
        return -1;

    /*
     * MMCEDRV must hook SIO2MAN before PAD/MEMCARD modules import it. The
     * bridge then exposes MMCEDRV through the same file-like API used by the
     * asset manager. Normal controller/save clients are loaded afterwards.
     */
    if (LOAD_IRX(sio2man) < 0 ||
        LOAD_IRX(mmcedrv) < 0 ||
        LOAD_IRX(ssb_mmce_stream) < 0)
        return -1;

    if (LOAD_IRX(mtapman) < 0 ||
        LOAD_IRX(padman) < 0 ||
        LOAD_IRX(mcman) < 0 ||
        LOAD_IRX(mcserv) < 0 ||
        LOAD_IRX(libsd) < 0)
        return -1;

    ps2_log("IOP: final MMCE game stack ready (MMCEDRV before PAD/MC)");
    return 0;
}

int ps2_iop_load_audio_driver(void)
{
    /*
     * Do not use sdrdrv here. Real-hardware testing proved that the SDR client
     * can poison the shared SIF RPC path and stall later storage traffic.
     * ssb_audio is a tiny purpose-built server exposing only the libsd
     * operations this port needs (init, batched register writes, sample DMA,
     * and optional readback).
     */
    if (ps2_iop_module_loaded("ssb_audio"))
        return 0;

    ps2_log("IOP: starting dedicated ssb_audio server");
    return LOAD_IRX(ssb_audio);
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    char ip_arg[24];

    /* A typed alias still refers to the inherited mount. Do not install a
     * second BDM core/transport on top of the launcher's registered device. */
    if (sInheritedBdm &&
        (dev == PS2_BOOT_BDM || dev == PS2_BOOT_USB ||
         dev == PS2_BOOT_ATA || dev == PS2_BOOT_MX4SIO ||
         dev == PS2_BOOT_ILINK || dev == PS2_BOOT_UDPBD))
    {
        ps2_log("IOP: using inherited %s transport", ps2_storage_device_name(dev));
        return 0;
    }

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_BDM:
    case PS2_BOOT_MC:
        return 0;

    case PS2_BOOT_CDROM:
        return LOAD_IRX(cdvd);

    case PS2_BOOT_USB:
        if (load_bdm_core() < 0 || LOAD_IRX(usbd_mini) < 0 || LOAD_IRX(usbmass_bd_mini) < 0)
            return -1;
        return 0;

    case PS2_BOOT_ATA:
        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0 || LOAD_IRX(ps2atad) < 0)
            return -1;
        return 0;

    case PS2_BOOT_MX4SIO:
        if (load_bdm_core() < 0 || LOAD_IRX(mx4sio_bd) < 0)
            return -1;
        return 0;

    case PS2_BOOT_ILINK:
        if (load_bdm_core() < 0 || LOAD_IRX(iLinkman) < 0 || LOAD_IRX(IEEE1394_bd) < 0)
            return -1;
        return 0;

    case PS2_BOOT_UDPBD:
        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0)
            return -1;
        if (!read_ip_arg(ip_arg, sizeof(ip_arg)))
            return -1;
        return LOAD_IRX_ARGS(smap_udpbd, ip_arg, (int)strlen(ip_arg) + 1);

    case PS2_BOOT_UDPFS:
        if (LOAD_IRX(ps2dev9) < 0)
            return -1;
        if (!read_ip_arg(ip_arg, sizeof(ip_arg)))
            return -1;
        if (LOAD_IRX(udpfs_smap) < 0)
            return -1;
        if (LOAD_IRX_ARGS(udpfs_ministack, ip_arg, (int)strlen(ip_arg) + 1) < 0)
            return -1;
        if (LOAD_IRX(udpfs_ioman) < 0)
            return -1;
        return 0;

    case PS2_BOOT_MMCE:
        return LOAD_IRX(mmceman);

    case PS2_BOOT_HDD:
    {
        static char hdd_args[] = "-o\0" "4\0" "-n\0" "20";
        static char pfs_args[] = "-o\0" "10\0" "-n\0" "40";

        /* A bare pfsN: data path has no APA partition name to remount.  It is
         * valid only when bootpath.c requested that the launcher's IOP/mount
         * be preserved; in that case the filesystem is already ready. */
        if (ps2_storage_hdd_mount_source()[0] == '\0')
        {
            if (ps2_storage_requires_iop_preserve())
            {
                ps2_log("IOP: using inherited PFS mount");
                return 0;
            }
            return -1;
        }

        if (LOAD_IRX(ps2dev9) < 0 || load_bdm_core() < 0 || LOAD_IRX(ps2atad) < 0)
            return -1;

        /* Proven launcHER/OSDMenu ordering: ATAD needs a short settle before
         * the APA driver probes the disk on real hardware. */
        sleep(1);

        if (LOAD_IRX_ARGS(ps2hdd, hdd_args, sizeof(hdd_args)) < 0)
            return -1;
        if (LOAD_IRX_ARGS(ps2fs, pfs_args, sizeof(pfs_args)) < 0)
            return -1;
        if (LOAD_IRX(secrsif) < 0)
            return -1;
        return mount_hdd_partition();
    }

    case PS2_BOOT_UNKNOWN:
    default:
        ps2_log("IOP: unsupported/ambiguous data device; refusing USB fallback");
        return -1;
    }
}

int ps2_iop_load_bdm_fallback_transports(void)
{
    static int sDone;
    if (sDone)
        return 0;
    sDone = 1;

    if (sInheritedBdm)
    {
        /* A failed probe does not authorize a second BDM core or replacing
         * the SIO2 hooks of an already-mounted transport. */
        ps2_log("IOP: preserving inherited BDM transports");
        return 0;
    }

    ps2_log("IOP: loading BDM fallback transports (USB/MX4SIO/ATA/iLink)");
    load_bdm_core();
    LOAD_IRX(usbd_mini);
    LOAD_IRX(usbmass_bd_mini);
    LOAD_IRX(mx4sio_bd);
    if (LOAD_IRX(ps2dev9) >= 0)
    {
        LOAD_IRX(ps2atad);
        sleep(1);
    }
    LOAD_IRX(iLinkman);
    LOAD_IRX(IEEE1394_bd);
    return 1;
}

int ps2_iop_module_loaded(const char *name)
{
    int i;

    for (i = 0; i < sLoadedCount; i++)
    {
        if (strcmp(sLoaded[i], name) == 0)
            return 1;
    }
    return 0;
}

int ps2_iop_module_count(void)
{
    return sLoadedCount;
}

const char *ps2_iop_module_name(int i)
{
    return (i >= 0 && i < sLoadedCount) ? sLoaded[i] : "";
}
