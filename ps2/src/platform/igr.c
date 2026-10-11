/*
 * Minimal IGR with no blocking memory-card probes or storage handoff.
 * Return to the system browser without blanking GS prematurely.
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

    /* Safety fallback: ExecOSD should not return. */
    LoadExecPS2("rom0:OSDSYS", 1, argv);
    for (;;)
        SleepThread();
}
