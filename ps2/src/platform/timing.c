/*
 * Time base and VBlank.
 *
 * The N64 build is driven by the VI retrace interrupt (60 Hz NTSC), which the
 * scheduler turns into game ticks. The PS2 port always runs the GS in an NTSC
 * mode, so the VBlank-start interrupt gives the same 59.94 Hz cadence and the
 * game keeps its original update rate regardless of how long rendering takes.
 */
#include <ps2/platform.h>

#include <kernel.h>
#include <delaythread.h>
#include <timer.h>

static volatile uint32_t sVBlankCount;
static int sVBlankHandlerId = -1;
static int sVBlankSema = -1;

/* Implemented by the libultra VI layer (ps2/src/ultra/vi.c). Runs in
 * interrupt context: applies pending framebuffer swaps, posts VI events. */
extern void ps2_vi_vblank_isr(void);

static s32 vblank_handler(s32 cause)
{
    (void)cause;
    sVBlankCount++;
    ps2_vi_vblank_isr();
    if (sVBlankSema >= 0)
    {
        iSignalSema(sVBlankSema);
    }
    ExitHandler();
    return 0;
}

void ps2_vblank_init(void)
{
    ee_sema_t sema = { 0 };

    sema.init_count = 0;
    sema.max_count = 1;
    sVBlankSema = CreateSema(&sema);
    if (sVBlankSema < 0)
        ps2_panic("timing: VBlank semaphore creation failed");

    sVBlankHandlerId = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    if (sVBlankHandlerId < 0)
        ps2_panic("timing: VBlank interrupt registration failed");
    EnableIntc(INTC_VBLANK_S);
}

uint32_t ps2_vblank_count(void)
{
    return sVBlankCount;
}

/* Block the calling thread for n VBlanks (boot-time helper only). */
void ps2_delay_vblanks(int n)
{
    if (n <= 0)
        return;
    DelayThread(n * 16667);
}

uint64_t ps2_time_ticks(void)
{
    return GetTimerSystemTime();
}

uint32_t ps2_time_us(void)
{
    /* 147.456 MHz bus clock -> microseconds */
    return (uint32_t)((GetTimerSystemTime() * 125u) / 18432u);
}
