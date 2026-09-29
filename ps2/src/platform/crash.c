/*
 * Crash reports.
 *
 * Any EE CPU exception that is not handled by the kernel (bad address,
 * misaligned access, bus error, reserved instruction, trap, ...) lands in
 * ps2_crash_vector (crash_vec.S), which saves the registers and continues
 * here on a private stack. The report re-initialises the display with
 * ps2sdk's debug text screen (480i, independent of the game renderer),
 * prints the exception, registers and the last log lines, then tries to
 * write the log to <boot dir>SSB64.LOG.
 *
 * On PCSX2 most of these exceptions are not raised at all (misaligned
 * accesses, TLB misses on unmapped addresses), which is why a build that
 * runs fine there can still stop on hardware.
 */
#include <ps2/platform.h>

#include <debug.h>
#include <kernel.h>
#include <string.h>

typedef struct CrashFrame
{
    uint64_t gpr[32][2];
    uint64_t hi[2];
    uint64_t lo[2];
    uint32_t status, cause, epc, badvaddr, errorepc;
    uint32_t pad[3];
} CrashFrame;

CrashFrame gPS2CrashFrame __attribute__((aligned(16)));
static uint8_t sCrashStack[16 * 1024] __attribute__((aligned(16)));
uint32_t gPS2CrashStackTop;
int gPS2CrashTest; /* debug: set to 1 to trigger a test crash (trap) */

extern void ps2_crash_vector(void);
extern int ps2_log_line_count(void);
extern const char *ps2_log_line(int i);

static const char *const sCauseNames[32] = {
    "interrupt", "TLB modified", "TLB miss (load/fetch)", "TLB miss (store)",
    "address error (load/fetch)", "address error (store)", "bus error (fetch)", "bus error (data)",
    "syscall", "break", "reserved instruction", "coprocessor unusable",
    "arithmetic overflow", "trap",
};

static const char *const sRegNames[32] = {
    "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

void ps2_crash_report(void)
{
    const CrashFrame *f = &gPS2CrashFrame;
    uint32_t code = (f->cause >> 2) & 31;
    const char *name = (code < 14 && sCauseNames[code]) ? sCauseNames[code] : "unknown";
    int self, i, n;

    DI();
    ps2_log_console(0);
    self = GetThreadId();
    /* keep the other threads (renderer, audio, game) from touching the
     * GS or the IOP while the report is shown */
    for (i = 1; i < 256; i++)
    {
        if (i != self)
        {
            SuspendThread(i);
        }
    }

    ps2_log("*** CRASH: %s (cause %u) ***", name, (unsigned)code);
    ps2_log("EPC %08x%s  BadVAddr %08x  Status %08x  ra %08x  sp %08x  thread %d", (unsigned)f->epc,
            (f->cause & 0x80000000u) ? " (branch delay)" : "", (unsigned)f->badvaddr, (unsigned)f->status,
            (unsigned)f->gpr[31][0], (unsigned)f->gpr[29][0], self);

    init_scr();
    scr_setbgcolor(0x400000);
    scr_clear();
    scr_printf("\n  Super Smash Bros. 64 (PS2 port) stopped: CPU exception\n\n");
    scr_printf("  %s (cause %u)\n", name, (unsigned)code);
    scr_printf("  EPC %08x%s   BadVAddr %08x   Status %08x   thread %d\n\n", (unsigned)f->epc,
               (f->cause & 0x80000000u) ? " (in branch delay slot)" : "", (unsigned)f->badvaddr,
               (unsigned)f->status, self);
    for (i = 0; i < 32; i += 4)
    {
        scr_printf("  %s %08x  %s %08x  %s %08x  %s %08x\n", sRegNames[i], (unsigned)f->gpr[i][0],
                   sRegNames[i + 1], (unsigned)f->gpr[i + 1][0], sRegNames[i + 2], (unsigned)f->gpr[i + 2][0],
                   sRegNames[i + 3], (unsigned)f->gpr[i + 3][0]);
    }
    scr_printf("\n  Last log lines:\n");
    n = ps2_log_line_count();
    for (i = (n > 10) ? n - 10 : 0; i < n; i++)
    {
        char line[76];

        strncpy(line, ps2_log_line(i), sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        scr_printf("  %s\n", line);
    }

    /* The screen is up; now try to leave a copy on the boot device (needs
     * interrupts for the IOP RPC, so this comes last). */
    EI();
    scr_printf("\n  Writing SSB64.LOG next to the ELF... ");
    ps2_log_save();
    scr_printf("done.\n");

    for (;;)
    {
        SleepThread();
    }
}

void ps2_crash_init(void)
{
    int i;

    gPS2CrashStackTop = (uint32_t)(uintptr_t)(sCrashStack + sizeof(sCrashStack) - 16);
    for (i = 1; i <= 3; i++)
    {
        SetVTLBRefillHandler(i, ps2_crash_vector);
    }
    for (i = 4; i <= 7; i++)
    {
        SetVCommonHandler(i, ps2_crash_vector);
    }
    SetVCommonHandler(10, ps2_crash_vector);
    SetVCommonHandler(12, ps2_crash_vector);
    SetVCommonHandler(13, ps2_crash_vector);
}

/* Called from the render thread loop (debug hook). */
void ps2_crash_test_poll(void)
{
    if (gPS2CrashTest)
    {
        gPS2CrashTest = 0;
        __asm__ volatile("teq $0, $0");
    }
}
