/*
 * PS2 entry point.
 *
 * Brings up the platform services in dependency order, then hands over to
 * the game's own boot path (syMainLoop, the N64 "thread 0" code), which
 * creates the idle/main/scheduler/audio/controller threads exactly as on the
 * N64 - through the libultra layer in ps2/src/ultra/.
 */
#include <ps2/platform.h>

#include <kernel.h>
#include <stdio.h>

/* Game side (src/sys/main.c). Weak so the platform can be built and booted
 * on its own while bringing up new hardware paths. */
extern void syMainLoop(void) __attribute__((weak));

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

#define PS2_BOOT_TITLE "Super Smash Bros. 64 - PS2 native port"

int main(int argc, char *argv[])
{
    const PS2MemStats *mem;

    ps2_log_init();
    ps2_mem_init();
    ps2_storage_set_boot_path((argc > 0) ? argv[0] : NULL);

    /* Boot with a high priority so init is not preempted by game threads
     * that get created along the way. */
    ChangeThreadPriority(GetThreadId(), 2);

    ps2_log("boot: argv0=%s", (argc > 0 && argv[0] != NULL) ? argv[0] : "(none)");
    ps2_log("boot: device=%s dir=%s", ps2_storage_device_name(ps2_storage_boot_device()), ps2_storage_boot_dir());

    ps2_iop_init();
    ps2_vblank_init();
    ps2_gs_init();
    ps2_gs_boot_screen(PS2_BOOT_TITLE);

    ps2_iop_load_boot_device_drivers(ps2_storage_boot_device());
    ps2_ultra_threads_init();
    ps2_vi_init();
    ps2_arena_init();
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

    if (syMainLoop == NULL)
    {
        ps2_log("boot: no game linked - platform self-test only");
        ps2_gs_boot_screen(PS2_BOOT_TITLE);
        for (;;)
        {
            SleepThread();
        }
    }

    ps2_log("boot: starting game");
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
