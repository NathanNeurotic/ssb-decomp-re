/*
 * Small libultra services: clock, caches, PI (ROM / SRAM "DMA"), SP task
 * submission, AI, and assorted globals.
 *
 * None of these touch fictitious N64 hardware. Cache maintenance existed to
 * keep the RCP coherent with the CPU; the RCP's work now runs on the EE and
 * the renderer/audio backends flush exactly what they hand to real PS2 DMA,
 * so the game's calls are no-ops.
 */
#include <ps2/ultra.h>
#include <ps2/ee_shim.h>
#include <ps2/platform.h>

#include <PR/os_internal.h>
#include <PR/rcp.h>
#include <PR/mbi.h>
#include <PR/sptask.h>


/* ------------------------------------------------------------------ */
/* Globals the game reads                                               */
/* ------------------------------------------------------------------ */

s32 osTvType = OS_TV_NTSC;
s32 osRomType = 0;
s32 osResetType = 0; /* cold reset */
s32 osAppNMIBuffer[16];
u32 osMemSize = 0x400000; /* the game only ever ran in 4 MiB */
u64 osClockRate = 62500000;

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void osInitialize(void)
{
}

void __osSetWatchLo(u32 value)
{
    (void)value;
}

s32 osAfterPreNMI(void)
{
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clock: N64 CP0 Count runs at 46.875 MHz; derive it from the EE bus clock
 * (147.456 MHz): count = ticks * 15625 / 49152. */
/* ------------------------------------------------------------------ */

static u64 sTimeBase;

u32 osGetCount(void)
{
    return (u32)((ps2_time_ticks() * 15625ull) / 49152ull);
}

OSTime osGetTime(void)
{
    return ((ps2_time_ticks() * 15625ull) / 49152ull) - sTimeBase;
}

void osSetTime(OSTime t)
{
    sTimeBase = ((ps2_time_ticks() * 15625ull) / 49152ull) - t;
}

/* ------------------------------------------------------------------ */
/* Caches (see file comment)                                            */
/* ------------------------------------------------------------------ */

void osWritebackDCache(void *vaddr, s32 nbytes)
{
    (void)vaddr;
    (void)nbytes;
}

void osWritebackDCacheAll(void)
{
}

void osInvalDCache(void *vaddr, s32 nbytes)
{
    (void)vaddr;
    (void)nbytes;
}

void osInvalICache(void *vaddr, s32 nbytes)
{
    (void)vaddr;
    (void)nbytes;
}

u32 osVirtualToPhysical(void *addr)
{
    /* Pointers stay pointers: every consumer of "physical" addresses (task
     * structures, display lists) is now EE code. */
    return (u32)(uintptr_t)addr;
}

/* ------------------------------------------------------------------ */
/* PI: cartridge ROM and SRAM                                           */
/* ------------------------------------------------------------------ */

/* Asset manager: serves N64 ROM address ranges from the PS2 asset pack. */
extern void ps2_rom_read(u32 rom_addr, void *dst, u32 size);
/* Save system: 32 KiB SRAM image backed by the memory card. */
extern void ps2_sram_read(u32 offset, void *dst, u32 size);
extern void ps2_sram_write(u32 offset, const void *src, u32 size);

static OSPiHandle sRomHandle;

OSPiHandle *osCartRomInit(void)
{
    sRomHandle.type = DEVICE_TYPE_CART;
    sRomHandle.baseAddress = 0xB0000000;
    return &sRomHandle;
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt)
{
    (void)pri;
    osCreateMesgQueue(cmdQ, cmdBuf, cmdMsgCnt);
}

s32 osEPiLinkHandle(OSPiHandle *handle)
{
    (void)handle;
    return 0;
}

s32 osEPiStartDma(OSPiHandle *handle, OSIoMesg *mb, s32 direction)
{
    if (handle->type == DEVICE_TYPE_SRAM)
    {
        /* SRAM lives at PI domain 2 address 0x08000000. */
        u32 offset = mb->devAddr & 0x0007FFFF;

        if (direction == OS_READ)
            ps2_sram_read(offset, mb->dramAddr, mb->size);
        else
            ps2_sram_write(offset, mb->dramAddr, mb->size);
    }
    else if (direction == OS_READ)
    {
        ps2_rom_read(mb->devAddr, mb->dramAddr, mb->size);
    }
    else
    {
        ps2_log("PI: ignored write to ROM 0x%08lx", (unsigned long)mb->devAddr);
    }

    if (mb->hdr.retQueue != NULL)
    {
        if (osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK) != 0)
        {
            ps2_log("PI: completion queue unexpectedly full");
            return -1;
        }
    }
    return 0;
}

s32 osEPiReadIo(OSPiHandle *handle, u32 devAddr, u32 *data)
{
    (void)handle;
    ps2_rom_read(devAddr, data, sizeof(u32));
    return 0;
}

OSMesgQueue *osPiGetCmdQueue(void)
{
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SP / DP                                                              */
/* ------------------------------------------------------------------ */

void osSpTaskLoad(OSTask *task)
{
    (void)task;
}

/* osSpTaskStart() is a macro: osSpTaskLoad() + osSpTaskStartGo(). */
void osSpTaskStartGo(OSTask *task)
{
    if (task->t.type == M_GFXTASK)
    {
        ps2_gfx_task_submit(task); /* posts OS_EVENT_SP + OS_EVENT_DP when done */
    }
    else
    {
        ps2_audio_task_run(task);  /* posts OS_EVENT_SP when done */
    }
}

/* PS2 tasks are not preemptible mid-list; the scheduler copes with a task
 * that "did not yield" (it simply finishes first). */
void osSpTaskYield(void)
{
}

OSYieldResult osSpTaskYielded(OSTask *task)
{
    (void)task;
    return 0;
}

s32 osDpSetNextBuffer(void *buf, u64 size)
{
    (void)buf;
    (void)size;
    return 0;
}

u32 osDpGetStatus(void)
{
    return 0;
}

void osDpSetStatus(u32 data)
{
    (void)data;
}

/* ------------------------------------------------------------------ */
/* AI: the N64 audio DAC. The SPU2 backend owns output; see ps2/src/audio. */
/* ------------------------------------------------------------------ */

extern s32 ps2_audio_ai_set_frequency(u32 frequency);
extern s32 ps2_audio_ai_set_next_buffer(void *buf, u32 size);
extern u32 ps2_audio_ai_get_length(void);

s32 osAiSetFrequency(u32 frequency)
{
    return ps2_audio_ai_set_frequency(frequency);
}

s32 osAiSetNextBuffer(void *buf, u32 size)
{
    return ps2_audio_ai_set_next_buffer(buf, size);
}

u32 osAiGetLength(void)
{
    return ps2_audio_ai_get_length();
}

u32 osAiGetStatus(void)
{
    return 0;
}

OSThread *__osGetActiveQueue(void)
{
    return NULL; /* crash-screen thread list: not available on PS2 */
}

/* Interrupt mask: the game only uses it for short critical sections
 * (osSetIntMask(OS_IM_NONE) ... osSetIntMask(prev)). */
OSIntMask osSetIntMask(OSIntMask mask)
{
    s32 was_enabled = ps2_intr_disable();

    if (mask != OS_IM_NONE)
    {
        EIntr();
    }
    return was_enabled ? OS_IM_ALL : OS_IM_NONE;
}

/* ------------------------------------------------------------------ */
/* IO_READ / IO_WRITE (PR/rcp.h): the handful of RCP registers the game
 * touches directly, answered with their meaning rather than emulated.  */
/* ------------------------------------------------------------------ */

u32 ps2_io_read(u32 addr)
{
    switch (addr)
    {
    case AI_LEN_REG:
        return osAiGetLength();
    case SP_IMEM_START:
        return 6103; /* main.c boot check: IMEM holds the expected ucode word */
    case SP_DMEM_START:
        return 0xFFFFFFFF; /* main.c boot check */
    default:
        ps2_log("IO_READ of unhandled RCP register 0x%08lx", (unsigned long)addr);
        return 0;
    }
}

void ps2_io_write(u32 addr, u32 data)
{
    ps2_log("IO_WRITE of unhandled RCP register 0x%08lx = 0x%08lx", (unsigned long)addr, (unsigned long)data);
}
