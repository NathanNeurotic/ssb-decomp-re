/*
 * Native PS2 in-game reset / exit.
 *
 * RiptOPL-style IGR is detected by the PS2 pad layer. Once requested, this
 * path raises itself above the game/render threads, stops rumble and audio,
 * lets any active GIF DMA finish, blanks the display, then returns to OSDSYS.
 *
 * Do not probe mc0:/mc1: with normal POSIX/fileXio calls here. Real hardware
 * proved that the old direct-mc probe can block after the MMCE/runtime IOP
 * handoff, leaving a black screen with SPU2 audio still latched.
 */
#include <ps2/platform.h>
#include <ps2/input.h>

#include <kernel.h>

void ps2_igr_exit(void)
{
    static volatile int sExiting;
    char *browser_argv[1] = { "BootBrowser" };

    if (sExiting)
    {
        for (;;)
            SleepThread();
    }
    sExiting = 1;

    /*
     * Stop normal EE scheduling before touching shared PAD/audio/GS state.
     * Priority 1 is above the game, render and save workers; interrupt
     * handlers can still complete while the exit path drains bounded work.
     */
    ChangeThreadPriority(GetThreadId(), 1);

    ps2_log("IGR: exit requested");

    /* Stop new pad/rumble activity first, then silence every SPU2 voice.
     * ps2_audio_shutdown() has a bounded RPC drain and cannot wait forever. */
    ps2_input_quiesce();
    ps2_audio_shutdown();

    /*
     * Blank only after audio/input cleanup. The previous implementation
     * blanked first and then entered a blocking mc0: probe, which exactly
     * produced the reported black screen + stuck audio failure.
     */
    ps2_log("IGR: returning to OSDSYS");
    ps2_gs_prepare_exec();

    ExecOSD(1, browser_argv);

    /* ExecOSD normally never returns. Keep a ROM fallback so an unexpected
     * return still has a second route out instead of parking on black. */
    LoadExecPS2("rom0:OSDSYS", 1, browser_argv);

    for (;;)
        SleepThread();
}
