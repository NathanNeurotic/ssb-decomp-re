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

#include <delaythread.h>
#define NEWLIB_PORT_AWARE
#include <fileio-common.h>
#include <kernel.h>
#include <sifrpc.h>
#include <string.h>

/*
 * Dedicated IGR FILEIO client/buffers live for the process lifetime. A timed
 * out SIF RPC may still complete later; static storage prevents that late DMA
 * from targeting a stack frame that no longer exists.
 */
static SifRpcClientData_t sIgrFileClient __attribute__((aligned(64)));
static struct _fio_open_arg sIgrOpenArg __attribute__((aligned(64)));
static int sIgrOpenResult __attribute__((aligned(64))) = -1;

static int mc_boot_probe(const char *path)
{
    int elapsed;

    /*
     * Do not use libc open()/fileXio on the exit path: a stalled storage RPC
     * must not make IGR itself unresponsive. Bind a private FILEIO client and
     * submit OPEN asynchronously. If MCMAN/SIO2 or FILEIO does not answer
     * within the bounded window, skip BOOT.ELF and return to OSDSYS.
     */
    memset(&sIgrFileClient, 0, sizeof(sIgrFileClient));
    for (elapsed = 0; elapsed < 250; elapsed++)
    {
        int rc = sceSifBindRpc(&sIgrFileClient, 0x80000001u, 0);

        if (rc < 0)
            return -1;
        if (sIgrFileClient.server != NULL)
            break;
        DelayThread(1000);
    }
    if (sIgrFileClient.server == NULL)
        return -1;

    memset(&sIgrOpenArg, 0, sizeof(sIgrOpenArg));
    sIgrOpenArg.mode = FIO_O_RDONLY;
    strncpy(sIgrOpenArg.name, path, sizeof(sIgrOpenArg.name) - 1);
    sIgrOpenResult = -1;

    if (sceSifCallRpc(&sIgrFileClient, FIO_F_OPEN, SIF_RPC_M_NOWAIT,
                      &sIgrOpenArg, sizeof(sIgrOpenArg),
                      &sIgrOpenResult, sizeof(sIgrOpenResult),
                      NULL, NULL) < 0)
        return -1;

    for (elapsed = 0; elapsed < 500; elapsed++)
    {
        if (!sceSifCheckStatRpc(&sIgrFileClient))
            return sIgrOpenResult >= 0 ? 1 : 0;
        DelayThread(1000);
    }

    ps2_log("IGR: %s probe timed out; skipping memory-card chainload", path);
    return -1;
}

static int chainload_boot_elf(const char *path)
{
    char *argv[1];

    argv[0] = (char *)path;
    ps2_log("IGR: launching %s", path);
    LoadExecPS2(path, 1, argv);

    /*
     * LoadExecPS2 normally never returns. If the kernel loader rejects the
     * target after our probe, returning here lets IGR try the next fallback
     * rather than invoking undefined behaviour through a false noreturn.
     */
    ps2_log("IGR: LoadExecPS2 returned for %s", path);
    return 0;
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

    {
        int mc0 = mc_boot_probe("mc0:/BOOT/BOOT.ELF");

        if (mc0 > 0)
            chainload_boot_elf("mc0:/BOOT/BOOT.ELF");
        /*
         * A timeout means the card/SIO2 path is unhealthy. Do not queue a
         * second card request behind it; preserve the guaranteed OSDSYS exit.
         */
        if (mc0 >= 0 && mc_boot_probe("mc1:/BOOT/BOOT.ELF") > 0)
            chainload_boot_elf("mc1:/BOOT/BOOT.ELF");
    }

    ps2_log("IGR: BOOT.ELF unavailable; returning to OSDSYS");
    ExecOSD(1, browser_argv);
    __builtin_unreachable();
}
