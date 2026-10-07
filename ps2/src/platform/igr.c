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

#include <fcntl.h>
#include <kernel.h>
#include <unistd.h>

static int mc_boot_exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    close(fd);
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
     * MCMAN exposes mc0:/mc1: through the normal filesystem API, so IGR does
     * not bind libmc/MCSERV either. Probe BOOT.ELF directly; a missing card or
     * file falls through to OSDSYS without introducing another synchronous
     * RPC dependency on the exit path.
     */
    if (mc_boot_exists("mc0:/BOOT/BOOT.ELF"))
        chainload_boot_elf("mc0:/BOOT/BOOT.ELF");
    if (mc_boot_exists("mc1:/BOOT/BOOT.ELF"))
        chainload_boot_elf("mc1:/BOOT/BOOT.ELF");

    ps2_log("IGR: BOOT.ELF unavailable; returning to OSDSYS");
    ExecOSD(1, browser_argv);
    __builtin_unreachable();
}
