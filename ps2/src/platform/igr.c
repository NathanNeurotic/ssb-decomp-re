/*
 * Native PS2 in-game reset / exit.
 *
 * RiptOPL-style IGR is detected by the PS2 pad layer. Once requested, this
 * path raises itself above the game/render threads, stops rumble, lets any
 * active GIF DMA finish, blanks the display, then leaves SSB64.
 *
 * Destination order:
 *   1. mc0:/BOOT/BOOT.ELF
 *   2. mc1:/BOOT/BOOT.ELF
 *   3. PS2 browser / OSDSYS
 */
#include <ps2/platform.h>
#include <ps2/input.h>

#include <kernel.h>
#include <libmc.h>

static int mc_call(int queued)
{
    int cmd;
    int result;

    if (queued != 0)
        return -1;
    mcSync(0, &cmd, &result);
    return result;
}

static int mc_boot_exists(int port)
{
    int fd = mc_call(mcOpen(port, 0, "/BOOT/BOOT.ELF", 1 /* O_RDONLY */));

    if (fd < 0)
        return 0;
    mc_call(mcClose(fd));
    return 1;
}

static void chainload_boot_elf(const char *path) __attribute__((noreturn));

static void chainload_boot_elf(const char *path)
{
    char *argv[1];

    argv[0] = (char *)path;
    ps2_log("IGR: launching %s", path);
    LoadExecPS2(path, 1, argv);
    __builtin_unreachable();
}

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
     * Stop normal EE scheduling before touching shared PAD/GS state. Priority
     * 1 is above the game, render and save workers; interrupt handlers can
     * still complete normally while we quiesce hardware.
     */
    ChangeThreadPriority(GetThreadId(), 1);

    ps2_log("IGR: exit requested");
    ps2_input_quiesce();
    ps2_gs_prepare_exec();

    /*
     * Save initialization normally brought libmc up already, but retrying it
     * here also covers boots where saving was unavailable earlier. We only
     * call LoadExecPS2 after confirming that BOOT.ELF can actually be opened,
     * otherwise we retain the guaranteed OSDSYS fallback.
     */
    if (mcInit(MC_TYPE_XMC) >= 0)
    {
        if (mc_boot_exists(0))
            chainload_boot_elf("mc0:/BOOT/BOOT.ELF");
        if (mc_boot_exists(1))
            chainload_boot_elf("mc1:/BOOT/BOOT.ELF");
    }

    ps2_log("IGR: BOOT.ELF unavailable; returning to OSDSYS");
    ExecOSD(1, browser_argv);
    __builtin_unreachable();
}
