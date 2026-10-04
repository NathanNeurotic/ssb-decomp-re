/*
 * PS2 entry point.
 *
 * Brings up the platform services in dependency order, then hands over to
 * the game's own boot path (syMainLoop, the N64 "thread 0" code), which
 * creates the idle/main/scheduler/audio/controller threads exactly as on the
 * N64 - through the libultra layer in ps2/src/ultra/.
 */
#include <ps2/platform.h>

#include <fcntl.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Game side (src/sys/main.c): the N64 boot thread's code. */
extern void syMainLoop(void);

/* Platform services implemented elsewhere. */
extern void ps2_gs_init(void);
extern void ps2_gs_boot_screen(const char *title);
extern void ps2_ultra_threads_init(void);
extern void ps2_vi_init(void);
extern void ps2_input_init(void);
extern int ps2_assets_init(void);
extern void ps2_save_init(void);
extern void ps2_audio_init(void);
extern void ps2_render_thread_init(void);
extern void ps2_arena_init(void);
extern void ps2_overlay_state_init(void);

#define PS2_BOOT_TITLE "Super Smash Bros. 64 - PS2 native port"

/* Physical/network storage can appear asynchronously after its drivers load.
 * Match launcHER's conservative real-hardware window: wait up to ~20 s. */
static int wait_for_boot_file(const char *name)
{
    extern void ps2_delay_vblanks(int n);
    char path[288];
    int i, fd = -1;

    /* newlib open() ultimately binds PS2SDK's basic FileIO RPC. Probe it
     * first in inherited-sidecar mode so a launcher without that service
     * fails visibly instead of entering fioInit()'s unbounded bind loop. */
    if (ps2_storage_requires_iop_preserve() &&
        !ps2_iop_rpc_available(0x80000001u))
    {
        ps2_log("boot: inherited FileIO RPC is unavailable");
        return 0;
    }

    for (i = 0; i < 200; i++)
    {
        /* Typed BDM paths (usb:/ata:/mx4sio:/ilink:/udpbd:) name the
         * transport, not necessarily the mounted filesystem. Resolve them
         * after the driver stack is resident, using this file as proof that
         * we selected the right massN: volume. */
        ps2_storage_resolve_data_root(name);
        ps2_storage_path(path, sizeof(path), name);
        fd = ps2_file_open_read(path);
        if (fd < 0 && ps2_storage_data_device() == PS2_BOOT_HOST)
        {
            /* PCSX2 Run ELF can pass a host argv[0] whose directory spelling
             * is not reusable; host: itself maps to the ELF directory. */
            snprintf(path, sizeof(path), "host:%s", name);
            fd = ps2_file_open_read(path);
        }
        if (fd >= 0)
        {
            ps2_file_close(fd);
            ps2_log("boot: %s found after %d ms", path, i * 100);
            return 1;
        }
        if (ps2_storage_data_device() == PS2_BOOT_HOST)
        {
            break; /* host: is there or not */
        }
        ps2_delay_vblanks(6);
    }
    ps2_log("boot: %s not found", path);
    return 0;
}

int ps2_main(int argc, char *argv[])
{
    const PS2MemStats *mem;
    int bad_data_arg = 0;
    int i;

    ps2_log_init();
    ps2_crash_init();
    ps2_storage_set_boot_path((argc > 0) ? argv[0] : NULL);

    /* Optional explicit asset/log directory. This is deliberately a directory
     * rather than a device switch: e.g. an ELF may live on mc0: while the
     * large SSB64.DAT lives on mass0:, udpfs:, MMCE or PFS. */
    for (i = 1; i < argc; i++)
    {
        if (argv[i] != NULL && strncmp(argv[i], "--data=", 7) == 0)
        {
            if (!ps2_storage_set_data_path(argv[i] + 7))
                bad_data_arg = 1;
        }
    }
    /* Stage colours (troubleshooting on hardware, see PS2_PORT.md):
     * dark blue = started, purple = IOP modules, cyan = video init,
     * after that the boot screen with the log is shown. */
    ps2_boot_stage("started", 0x000080);
    ps2_mem_init();

    /* Boot with a high priority so init is not preempted by game threads
     * that get created along the way. */
    ChangeThreadPriority(GetThreadId(), 2);

    ps2_log("boot: argv0=%s", (argc > 0 && argv[0] != NULL) ? argv[0] : "(none)");
    ps2_log("boot: launch=%s data=%s dir=%s",
            ps2_storage_device_name(ps2_storage_launch_device()),
            ps2_storage_device_name(ps2_storage_data_device()),
            ps2_storage_boot_dir());

    {
        /* Display lists address memory through RSP segments; a real EE
         * pointer at or above 16 MiB would alias segment 1. Everything the
         * game can reference (the ELF image incl. the scene arena) must stay
         * below that line; platform-only buffers may live above it. */
        extern char _end[];

        if ((uintptr_t)_end >= 0x01000000u)
        {
            ps2_panic("ELF image ends at %p, above the 16 MiB segment limit", _end);
        }
    }

    /* Bring the GS up before any inherited IOP/RPC binding. Hardware boot
     * failures are then visible as log text instead of an ambiguous solid
     * color, and video initialization cannot be taken down by a launcher IOP. */
    ps2_vblank_init();
    ps2_gs_init();
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    ps2_log("boot: initializing IOP/RPC handoff");
    ps2_gs_boot_screen(PS2_BOOT_TITLE);
    ps2_iop_init();
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    if (bad_data_arg)
        ps2_panic("invalid --data path; use a supported device directory (for example mass0:/SSB64/)");

    if (ps2_storage_data_device() == PS2_BOOT_UNKNOWN)
        ps2_panic("unsupported or ambiguous launch/data path: %s", ps2_storage_launch_path());

    if (ps2_iop_load_boot_device_drivers(ps2_storage_data_device()) < 0)
        ps2_panic("failed to initialize %s storage", ps2_storage_device_name(ps2_storage_data_device()));

    ps2_gs_boot_screen(PS2_BOOT_TITLE);
    if (!wait_for_boot_file("SSB64.DAT"))
        ps2_panic("asset pack is not reachable at %sSSB64.DAT", ps2_storage_boot_dir());

    /* Only after the real data pack has opened may optional/secondary runtime
     * services touch the inherited IOP. */
    if (ps2_iop_prepare_runtime_services() < 0)
        ps2_panic("required controller IOP services are unavailable");

    ps2_gs_boot_screen(PS2_BOOT_TITLE);
    if (ps2_storage_data_device() != PS2_BOOT_CDROM)
    {
        ps2_log_enable_save(1);
        ps2_log_save();
    }
    ps2_ultra_threads_init();
    ps2_vi_init();
    ps2_arena_init();
    ps2_overlay_state_init();
    ps2_input_init();
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    if (!ps2_assets_init())
    {
        ps2_panic("asset pack not found next to the ELF (%sSSB64.DAT). Run ps2/tools/prepare_assets.sh first.",
                  ps2_storage_boot_dir());
    }
    ps2_save_init();
    ps2_audio_init();
    ps2_render_thread_init();

    mem = ps2_mem_stats();
    ps2_log("mem: %u KiB committed (code/static %u KiB), budget %u KiB", (unsigned)(mem->total_used >> 10),
            (unsigned)(mem->used[PS2_MEM_CODE_STATIC] >> 10), (unsigned)(mem->budget >> 10));
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    ps2_log("boot: starting game");
    ps2_log_save();
    /* syMainLoop creates the idle thread (libultra priority 127), which in
     * turn starts the game's main thread; this boot thread then just parks
     * at the lowest priority. */
    syMainLoop();
    ChangeThreadPriority(GetThreadId(), 127);
    for (;;)
    {
        SleepThread();
    }
    return 0;
}
