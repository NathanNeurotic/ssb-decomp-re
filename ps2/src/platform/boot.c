/*
 * PS2 entry point.
 *
 * Brings up the platform services in dependency order, then hands over to
 * the game's own boot path (syMainLoop, the N64 "thread 0" code), which
 * creates the idle/main/scheduler/audio/controller threads exactly as on the
 * N64 - through the libultra layer in ps2/src/ultra/.
 */
#include <ps2/platform.h>

#include <delaythread.h>
#include <fcntl.h>
#include <kernel.h>
#include <stdio.h>
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

/* Raw GS display registers used only for hardware boot diagnostics.  These
 * markers deliberately bypass ps2_log(), ps2_boot_stage() and gsKit. */
#define PS2_BOOT_PMODE   (*(volatile uint64_t *)0x12000000)
#define PS2_BOOT_BGCOLOR (*(volatile uint64_t *)0x120000E0)
#define PS2_BOOT_PMODE_BG_ONLY ((uint64_t)(1u << 2))

static void ps2_boot_raw_color(uint8_t r, uint8_t g, uint8_t b)
{
    PS2_BOOT_PMODE = PS2_BOOT_PMODE_BG_ONLY;
    PS2_BOOT_BGCOLOR = (uint64_t)r | ((uint64_t)g << 8) | ((uint64_t)b << 16);
    __asm__ volatile("sync.p" ::: "memory");
}

/* USB (and MMCE) storage appears asynchronously after its drivers load;
 * wait until a file next to the ELF can be opened (up to ~6 s). */
static void wait_for_boot_file(const char *name)
{
    extern void ps2_delay_vblanks(int n);
    char path[288];
    int i, fd = -1;

    ps2_storage_path(path, sizeof(path), name);
    for (i = 0; i < 60; i++)
    {
        fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            close(fd);
            ps2_log("boot: %s found after %d ms", path, i * 100);
            return;
        }
        if (ps2_storage_boot_device() == PS2_BOOT_HOST)
        {
            break; /* host: is there or not */
        }
        ps2_delay_vblanks(6);
    }
    ps2_log("boot: %s not found", path);
}

int ps2_main(int argc, char *argv[])
{
    const PS2MemStats *mem;

    ps2_log_init();
    ps2_crash_init();
    ps2_storage_set_boot_path((argc > 0) ? argv[0] : NULL);
    /* Stage colours (troubleshooting on hardware, see PS2_PORT.md):
     * dark blue = started, purple = IOP modules, cyan/blue/yellow/green
     * = progressively later GS init stages, then the boot log screen. */
    ps2_boot_stage("started", 0x000080);
    ps2_mem_init();

    /* Boot with a high priority so init is not preempted by game threads
     * that get created along the way. */
    ChangeThreadPriority(GetThreadId(), 2);

    ps2_log("boot: argv0=%s", (argc > 0 && argv[0] != NULL) ? argv[0] : "(none)");
    ps2_log("boot: device=%s dir=%s", ps2_storage_device_name(ps2_storage_boot_device()), ps2_storage_boot_dir());

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

    ps2_boot_stage("IOP reset + modules", 0x800080);
    ps2_iop_init();
    /* Bring the GS/DMAC up before installing our VBlank ISR.  gsKit resets
     * and reprograms the GS during init; letting a game-side VBlank handler
     * run across that transition is unnecessary and can expose launcher/
     * hardware state that PCSX2's host: path does not reproduce. */
    ps2_boot_stage("GS video init", 0x008080);

    /* Hardware call-boundary probe.  Hold white long enough to be visible,
     * then leave red immediately before entering ps2_gs_init().  If a test
     * build still shows only the previous cyan marker, it is not executing
     * this ELF/code path.  If it freezes red, the call itself/fuction entry
     * is the failing boundary. */
    ps2_boot_raw_color(0xFF, 0xFF, 0xFF);
    DelayThread(500000);
    ps2_boot_raw_color(0xFF, 0x00, 0x00);
    DelayThread(250000);

    ps2_gs_init();
    ps2_vblank_init();
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    ps2_iop_load_boot_device_drivers(ps2_storage_boot_device());
    ps2_gs_boot_screen(PS2_BOOT_TITLE);
    wait_for_boot_file("SSB64.DAT");
    if (ps2_storage_boot_device() != PS2_BOOT_CDROM)
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
