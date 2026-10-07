/*
 * Minimal native PS2 in-game reset.
 *
 * Deliberately does not probe mc0:/BOOT or touch the active storage backend.
 * The previous path blacked the GS and then entered storage/RPC work, which
 * could strand the game on a black screen with audio latched. This path
 * quiesces pad/audio state and hands directly to the ROM browser.
 */
#include <ps2/platform.h>
#include <ps2/input.h>
#include <ps2/spu.h>

#include <kernel.h>

void ps2_igr_exit(void)
{
    static volatile int sExiting;
    char *argv[1] = { "BootBrowser" };

    if (sExiting)
    {
        for (;;)
            SleepThread();
    }
    sExiting = 1;

    ChangeThreadPriority(GetThreadId(), 1);
    ps2_input_quiesce();
    ps2_spu_shutdown();

    ExecOSD(1, argv);

    /* Fallback only if ExecOSD unexpectedly returns. */
    LoadExecPS2("rom0:OSDSYS", 1, argv);

    for (;;)
        SleepThread();
}
