/*
 * IOP bring-up for real launch/data devices.
 *
 * Normal launches keep the launcher's live IOP/filesystem because SSB64.DAT
 * is a sidecar beside SSB64.ELF. An explicit --data override is the only path
 * that rebuilds a separate data-device stack from a clean IOP.
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
#include <fileXio_rpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <io_common.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
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

DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(mmceman);
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

enum
{
    PS2_FS_CLIENT_NONE = 0,
    PS2_FS_CLIENT_FILEIO,
    PS2_FS_CLIENT_FILEXIO
};

static int sFsClient;
static int sBdmRecoveryStage;
static int sBdmRecoveryBaseReady;
static int sSaveServicesReady;
static int sAudioServicesReady;

static int load_irx(const char *name, void *buf, unsigned int size, const char *args, int args_len)
{
    int result = 0;
    int id = SifExecModuleBuffer(buf, size, (u32)args_len, args, &result);

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

int ps2_iop_rpc_available(uint32_t rpc_id)
{
    SifRpcClientData_t client __attribute__((aligned(64)));
    int attempt;

    memset(&client, 0, sizeof(client));
    for (attempt = 0; attempt < 500; attempt++)
    {
        if (sceSifBindRpc(&client, rpc_id, 0) < 0)
            return 0;
        if (client.server != NULL)
            return 1;
        DelayThread(1000);
    }
    return 0;
}

static int lazy_load_bridge_irx(const char *label, void *buf, unsigned int size)
{
    int result = 0;
    int id;

    /* RiptOPL/wLaunchELF use this same pattern: keep the live device stack,
     * initialize only the loader/heap RPC clients, enable LoadModuleBuffer,
     * then inject the missing bridge module. No IOP reset, no storage-driver
     * reload, and no mount teardown. */
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    id = SifExecModuleBuffer(buf, size, 0, NULL, &result);

    SifExitIopHeap();
    SifLoadFileExit();

    if (id < 0 || result == 1)
    {
        ps2_log("IOP: lazy %s load returned id=%d res=%d", label, id, result);
        return -1;
    }

    ps2_log("IOP: lazy-loaded %s", label);
    return 0;
}

/* Recovery-only loader. A launcher may have reset the IOP after loading the
 * ELF, or may have left some of these modules resident already. The caller
 * validates success by probing the resulting RPC/filesystem, so NO_RESIDENT
 * from a duplicate module is not fatal here. */
static int recovery_load_irx(const char *label, void *buf, unsigned int size)
{
    int result = 0;
    int id;

    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    id = SifExecModuleBuffer(buf, size, 0, NULL, &result);

    SifExitIopHeap();
    SifLoadFileExit();

    if (id < 0 && result != 1)
    {
        ps2_log("IOP: recovery %s load returned id=%d res=%d", label, id, result);
        return -1;
    }

    if (result == 1)
        ps2_log("IOP: recovery %s already resident/unneeded", label);
    else
        ps2_log("IOP: recovery-loaded %s", label);
    return 0;
}

static int device_prefers_iomanx(PS2BootDevice dev)
{
    switch (dev)
    {
    case PS2_BOOT_BDM:
    case PS2_BOOT_USB:
    case PS2_BOOT_ATA:
    case PS2_BOOT_MX4SIO:
    case PS2_BOOT_ILINK:
    case PS2_BOOT_UDPBD:
    case PS2_BOOT_UDPFS:
    case PS2_BOOT_HDD:
    case PS2_BOOT_MMCE:
        return 1;
    default:
        return 0;
    }
}

static int activate_filexio_client(void)
{
    if (sFsClient == PS2_FS_CLIENT_FILEXIO)
        return 0;

    /* Call this only after the FILEXIO module has been positively established
     * by an inherited-service path or a concrete module load. Do not perform
     * another RPC-ID presence probe here: probing a missing service is the
     * exact failure mode seen on real hardware. */
    ps2_log("IOP: binding fileXio client to established server");
    if (fileXioInit() < 0)
        return -1;

    sFsClient = PS2_FS_CLIENT_FILEXIO;
    ps2_log("IOP: filesystem client = fileXio/iomanX");
    return 0;
}

int ps2_iop_prepare_filesystem_client(void)
{
    const uint32_t fileio_rpc = 0x80000001u;
    PS2BootDevice dev = ps2_storage_data_device();
    int have_filexio = ps2_iop_rpc_available(FILEXIO_IRX);
    int have_fileio = ps2_iop_rpc_available(fileio_rpc);

    ps2_log("IOP: fs RPC fileXio=%s FileIO=%s",
            have_filexio ? "yes" : "no",
            have_fileio ? "yes" : "no");

    /* BDM/PFS/MMCE are iomanX filesystems. host:/mc:/cdrom: are commonly
     * exposed through the legacy FileIO/ioman service. Prefer the bridge that
     * matches the inherited device instead of assuming one RPC model for all
     * launchers (PCSX2 is a useful legacy-FileIO control case). */
    if (device_prefers_iomanx(dev) && have_filexio)
        return activate_filexio_client();

    if (have_fileio)
    {
        sFsClient = PS2_FS_CLIENT_FILEIO;
        ps2_log("IOP: filesystem client = FileIO/ioman");
        return 0;
    }

    if (have_filexio)
        return activate_filexio_client();

    sFsClient = PS2_FS_CLIENT_NONE;
    ps2_log("IOP: no inherited filesystem RPC client is currently reachable");
    return -1;
}

int ps2_iop_promote_filesystem_client(void)
{
    PS2BootDevice dev = ps2_storage_data_device();

    if (!ps2_storage_requires_iop_preserve() || !device_prefers_iomanx(dev))
        return 0;
    if (sFsClient == PS2_FS_CLIENT_FILEXIO)
        return 0;

    /* The real DAT open already failed through legacy FileIO. Do not ask
     * whether FILEXIO exists by binding to its RPC ID: an absent service can
     * wedge the EE before recovery even begins. Instead, concretely establish
     * the bridge. recovery_load_irx() tolerates an already-resident module,
     * so this covers both inherited fileXio and the missing-bridge case
     * without a speculative bind. */
    ps2_log("IOP: DAT not visible through FileIO; establishing fileXio bridge");
    if (recovery_load_irx("filexio", filexio_irx, size_filexio_irx) < 0)
    {
        ps2_log("IOP: fileXio bridge not ready; deferring to BDM recovery");
        return -1;
    }

    if (activate_filexio_client() < 0)
    {
        ps2_log("IOP: fileXio client bind failed; deferring to BDM recovery");
        return -1;
    }

    ps2_log("IOP: promoted filesystem client to fileXio");
    return 1;
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

    sLoadedCount = 0;
    sIopWasReset = 0;
    sFsClient = PS2_FS_CLIENT_NONE;
    sBdmRecoveryStage = 0;
    sBdmRecoveryBaseReady = 0;
    sSaveServicesReady = 0;
    sAudioServicesReady = 0;

    /* Every child initializes its own EE-side SIF RPC client state. This does
     * not reset or alter the inherited IOP. */
    SifInitRpc(0);

    if (preserve_iop)
    {
        /* Sidecar contract: the launcher already proved this filesystem by
         * loading SSB64.ELF from it. Keep that IOP untouched. Basic POSIX file
         * calls in PS2SDK bind the inherited FileIO RPC lazily, so fileXio is
         * not a prerequisite for opening the adjacent SSB64.DAT. In
         * particular, do not initialize loadfile/iopheap, apply SBV patches,
         * reset the IOP, query BDM slots, or inject storage/bridge IRXs here. */
        ps2_log("IOP: keeping launcher sidecar device stack untouched");
        return;
    }

    /* Explicit cross-device --data mode is the only path that owns the IOP.
     * It starts from a clean state and loads one known stack. */
    fileXioExit();
    SifExitRpc();
    SifInitRpc(0);

    while (!SifIopReset("", 0))
    {
    }
    while (!SifIopSync())
    {
    }
    SifInitRpc(0);
    sIopWasReset = 1;

    SifLoadFileInit();
    SifInitIopHeap();

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    LOAD_IRX(iomanx);
    LOAD_IRX(filexio);
    fileXioInit();

    LOAD_IRX(sio2man);
    LOAD_IRX(mtapman);
    LOAD_IRX(padman);

    /* Memory-card and sound services are intentionally deferred until after
     * SSB64.DAT and controller input are proven. This keeps optional service
     * failures from taking storage or boot down with them. */
    ps2_log("IOP: reset/rebuilt, %d base modules", sLoadedCount);
}

int ps2_iop_recover_generic_bdm_next(void)
{
    const uint32_t loadfile_rpc = 0x80000006u;
    const uint32_t iopheap_rpc = 0x80000003u;

    if (ps2_storage_data_device() != PS2_BOOT_BDM ||
        !ps2_storage_requires_iop_preserve())
        return 0;

    if (!ps2_iop_rpc_available(loadfile_rpc) ||
        !ps2_iop_rpc_available(iopheap_rpc))
    {
        ps2_log("IOP: generic BDM recovery unavailable (loader/heap RPC missing)");
        return -1;
    }

    if (!sBdmRecoveryBaseReady)
    {
        ps2_log("IOP: inherited massN: is gone; rebuilding BDM core without IOP reset");

        /* A reset launcher leaves legacy ioman/FileIO but removes the
         * iomanX/BDM stack. Try fileXio first in case iomanX survived; if it
         * did not, add iomanX and then the EE<->IOP bridge. */
        if (!ps2_iop_rpc_available(FILEXIO_IRX))
        {
            recovery_load_irx("filexio", filexio_irx, size_filexio_irx);
            if (!ps2_iop_rpc_available(FILEXIO_IRX))
            {
                recovery_load_irx("iomanx", iomanx_irx, size_iomanx_irx);
                recovery_load_irx("filexio", filexio_irx, size_filexio_irx);
            }
        }

        if (!ps2_iop_rpc_available(FILEXIO_IRX) ||
            activate_filexio_client() < 0)
        {
            ps2_log("IOP: generic BDM recovery could not establish fileXio");
            return -1;
        }

        /* Duplicate loads are tolerated here because the real proof is the
         * sidecar DAT open, never module bookkeeping. */
        recovery_load_irx("bdm", bdm_irx, size_bdm_irx);
        recovery_load_irx("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
        sBdmRecoveryBaseReady = 1;
    }

    /* Generic massN: does not encode its transport. Add one transport family
     * at a time and let bootpath.c rediscover the real sidecar by relative
     * path. massN numbering after recovery is deliberately ignored. */
    switch (sBdmRecoveryStage++)
    {
    case 0:
        ps2_log("IOP: generic BDM recovery stage USB");
        recovery_load_irx("usbd_mini", usbd_mini_irx, size_usbd_mini_irx);
        recovery_load_irx("usbmass_bd_mini", usbmass_bd_mini_irx, size_usbmass_bd_mini_irx);
        return 1;

    case 1:
        ps2_log("IOP: generic BDM recovery stage MX4SIO");
        /* MX4SIO hooks SIO2MAN when present. After a launcher-side IOP reset
         * there is no live mass filesystem left to protect, so providing
         * SIO2MAN here is safe and also prepares later controller use. */
        recovery_load_irx("sio2man", sio2man_irx, size_sio2man_irx);
        recovery_load_irx("mx4sio_bd", mx4sio_bd_irx, size_mx4sio_bd_irx);
        return 1;

    case 2:
        ps2_log("IOP: generic BDM recovery stage iLink");
        recovery_load_irx("iLinkman", iLinkman_irx, size_iLinkman_irx);
        recovery_load_irx("IEEE1394_bd", IEEE1394_bd_irx, size_IEEE1394_bd_irx);
        return 1;

    case 3:
        ps2_log("IOP: generic BDM recovery stage ATA");
        recovery_load_irx("ps2dev9", ps2dev9_irx, size_ps2dev9_irx);
        recovery_load_irx("ps2atad", ps2atad_irx, size_ps2atad_irx);
        return 1;

    default:
        return 0;
    }
}

static int pad_rpc_ready(void)
{
    const uint32_t pad1_new = 0x80000100u;
    const uint32_t pad2_new = 0x80000101u;
    const uint32_t pad1_old = 0x8000010Fu;
    const uint32_t pad2_old = 0x8000011Fu;

    return
        (ps2_iop_rpc_available(pad1_new) && ps2_iop_rpc_available(pad2_new)) ||
        (ps2_iop_rpc_available(pad1_old) && ps2_iop_rpc_available(pad2_old));
}

static int mtap_rpc_ready(void)
{
    const uint32_t mtap_open = 0x80000901u;
    const uint32_t mtap_close = 0x80000902u;
    const uint32_t mtap_conn = 0x80000903u;

    return ps2_iop_rpc_available(mtap_open) &&
           ps2_iop_rpc_available(mtap_close) &&
           ps2_iop_rpc_available(mtap_conn);
}

static void prepare_optional_mtap_service(void)
{
    if (mtap_rpc_ready())
    {
        ps2_log("IOP: inherited multitap RPC ready");
        return;
    }

    ps2_log("IOP: multitap RPC absent; lazy-loading mtapman");
    if (lazy_load_bridge_irx("mtapman", mtapman_irx, size_mtapman_irx) < 0)
    {
        ps2_log("IOP: multitap service unavailable; continuing with two native ports");
        return;
    }

    if (mtap_rpc_ready())
        ps2_log("IOP: multitap RPC ready");
    else
        ps2_log("IOP: mtapman loaded but RPC unavailable; continuing without multitap");
}

static int sio2_fallback_is_safe(void)
{
    PS2BootDevice dev = ps2_storage_data_device();

    switch (dev)
    {
    case PS2_BOOT_HOST:
    case PS2_BOOT_USB:
    case PS2_BOOT_ATA:
    case PS2_BOOT_ILINK:
    case PS2_BOOT_UDPBD:
    case PS2_BOOT_UDPFS:
    case PS2_BOOT_HDD:
    case PS2_BOOT_CDROM:
        return 1;

    case PS2_BOOT_BDM:
    {
        char driver[32];

        if (!ps2_storage_inherited_bdm_driver(driver, sizeof(driver)))
        {
            ps2_log("IOP: BDM transport unknown; refusing to replace SIO2");
            return 0;
        }

        ps2_log("IOP: inherited BDM transport=%s", driver);
        if (strcmp(driver, "sdc") == 0 || strcmp(driver, "mx4sio") == 0)
            return 0;

        return strcmp(driver, "usb") == 0 ||
               strcmp(driver, "ata") == 0 ||
               strcmp(driver, "sd") == 0 ||
               strcmp(driver, "ilink") == 0 ||
               strcmp(driver, "udp") == 0;
    }

    case PS2_BOOT_MC:
    case PS2_BOOT_MX4SIO:
    case PS2_BOOT_MMCE:
    case PS2_BOOT_UNKNOWN:
    default:
        return 0;
    }
}

int ps2_iop_prepare_runtime_services(void)
{
    /* Storage is already proven at this point because SSB64.DAT opened from
     * the sidecar path. Only controller RPC is mandatory for gameplay.
     *
     * First try to consume whatever pad stack the launcher left behind. If it
     * is absent, try PADMAN alone so launchers with a compatible live SIO2
     * service keep complete ownership of that transport. Only when PADMAN
     * still cannot register do we add SIO2MAN, and only for devices where
     * replacing/adding SIO2 cannot disconnect the active storage path. */
    if (ps2_storage_requires_iop_preserve())
    {
        if (pad_rpc_ready())
        {
            ps2_log("IOP: inherited pad RPC ready");
            prepare_optional_mtap_service();
            return 0;
        }

        ps2_log("IOP: pad RPC absent; trying padman on inherited SIO2");
        if (lazy_load_bridge_irx("padman", padman_irx, size_padman_irx) >= 0 &&
            pad_rpc_ready())
        {
            ps2_log("IOP: pad RPC ready via inherited SIO2");
            prepare_optional_mtap_service();
            return 0;
        }

        if (!sio2_fallback_is_safe())
        {
            ps2_log("IOP: controller RPC missing and SIO2 is storage-owned/unknown");
            return -1;
        }

        /* PS2SDK's reference pad sample loads SIO2MAN before PADMAN. PCSX2
         * host launches commonly arrive without either service, while USB,
         * ATA, iLink, UDP and HDD storage do not depend on SIO2. */
        ps2_log("IOP: adding sio2man + padman controller stack");
        if (lazy_load_bridge_irx("sio2man", sio2man_irx, size_sio2man_irx) < 0)
            return -1;
        if (lazy_load_bridge_irx("padman", padman_irx, size_padman_irx) < 0)
            return -1;

        if (!pad_rpc_ready())
        {
            ps2_log("IOP: pad RPC unavailable after sio2man + padman");
            return -1;
        }

        ps2_log("IOP: pad RPC ready after sio2man + padman");
        prepare_optional_mtap_service();
    }

    return 0;
}

static int load_rom_service(const char *label, const char *path)
{
    int id;

    /* By the time late services are prepared, LOADFILE has already been used
     * successfully by storage/controller recovery. Avoid another generic RPC
     * bind probe here: the real-hardware tests showed that service-presence
     * probing itself can be the thing that wedges. */
    ps2_log("IOP: loading ROM %s from %s", label, path);
    id = SifLoadModule(path, 0, NULL);
    ps2_log("IOP: ROM %s load returned %d", label, id);
    return id;
}

int ps2_iop_prepare_save_services(void)
{
    int rom_man;
    int rom_serv;
    int emb_man;
    int emb_serv;

    sSaveServicesReady = 0;

    if (!pad_rpc_ready())
    {
        ps2_log("IOP: memory-card recovery skipped; SIO2/pad stack not ready");
        return 0;
    }

    /* Do not probe 0x80000400 first. On real hardware, even a supposedly
     * asynchronous bind can wedge depending on the current SIF/RPC state.
     * Instead, make the service exist and let mcInit() be the authority. */
    ps2_log("IOP: preparing memory-card modules directly");
    rom_man = load_rom_service("XMCMAN", "rom0:XMCMAN");
    rom_serv = load_rom_service("XMCSERV", "rom0:XMCSERV");

    if (rom_man >= 0 && rom_serv >= 0)
    {
        sSaveServicesReady = 1;
        ps2_log("IOP: ROM memory-card modules started");
        return 1;
    }

    /* Negative ROM load results can mean an already-resident module or an
     * unavailable BIOS module. The PS2SDK embedded XMC pair is safe to try on
     * the already-proven SIO2 stack; NO_RESIDENT_END is treated as success by
     * recovery_load_irx(), so an existing compatible pair is accepted too. */
    ps2_log("IOP: ROM XMC pair not conclusively started; trying embedded XMC pair");
    emb_man = recovery_load_irx("mcman", mcman_irx, size_mcman_irx);
    emb_serv = recovery_load_irx("mcserv", mcserv_irx, size_mcserv_irx);

    if (emb_man >= 0 && emb_serv >= 0)
    {
        sSaveServicesReady = 1;
        ps2_log("IOP: memory-card modules ready for mcInit");
        return 1;
    }

    ps2_log("IOP: memory-card modules unavailable; persistence disabled");
    return 0;
}

int ps2_iop_save_services_ready(void)
{
    return sSaveServicesReady;
}

int ps2_iop_prepare_audio_services(void)
{
    int rom_libsd;
    int rom_sdr;
    int emb_sdr;
    int emb_libsd;

    sAudioServicesReady = 0;

    /* Same rule as memory cards: do not "test-bind" the optional service.
     * Start the canonical modules first, then allow the real libsdr client to
     * bind once there is a concrete server that should answer. */
    ps2_log("IOP: preparing audio modules directly");
    rom_libsd = load_rom_service("LIBSD", "rom0:LIBSD");
    rom_sdr = load_rom_service("SDRDRV", "rom0:SDRDRV");

    if (rom_sdr >= 0)
    {
        sAudioServicesReady = 1;
        ps2_log("IOP: ROM SDRDRV started");
        return 1;
    }

    /* If LIBSD is already resident (or its duplicate load returned an error),
     * an embedded SDR server can still use it. recovery_load_irx accepts an
     * already-resident SDR module as ready as well. */
    ps2_log("IOP: ROM SDRDRV not conclusively started; trying embedded SDR");
    emb_sdr = recovery_load_irx("sdr", sdr_irx, size_sdr_irx);
    if (emb_sdr >= 0)
    {
        sAudioServicesReady = 1;
        ps2_log("IOP: SDR server ready");
        return 1;
    }

    /* Final compatibility path for BIOSes without a usable LIBSD. */
    if (rom_libsd < 0)
    {
        ps2_log("IOP: trying FreeSD + embedded SDR fallback");
        emb_libsd = recovery_load_irx("libsd", libsd_irx, size_libsd_irx);
        emb_sdr = recovery_load_irx("sdr", sdr_irx, size_sdr_irx);
        if (emb_libsd >= 0 && emb_sdr >= 0)
        {
            sAudioServicesReady = 1;
            ps2_log("IOP: FreeSD/SDR fallback ready");
            return 1;
        }
    }

    ps2_log("IOP: no audio service could be started; audio stays silent");
    return 0;
}

int ps2_iop_audio_services_ready(void)
{
    return sAudioServicesReady;
}

int ps2_iop_load_boot_device_drivers(PS2BootDevice dev)
{
    char ip_arg[24];

    /* In sidecar/inherited mode the launcher's live filesystem is already the
     * correct device stack. Loading a second mmceman/BDM/network stack is both
     * unnecessary and unsafe. */
    if (ps2_storage_requires_iop_preserve())
        return 0;

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
