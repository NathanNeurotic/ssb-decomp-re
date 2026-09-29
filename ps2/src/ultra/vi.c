/*
 * VI (video interface) semantics on the GS.
 *
 * The game addresses framebuffers by their RDRAM pointers (gSYFramebufferSets)
 * and swaps with osViSwapBuffer(). The renderer maps each such pointer to a
 * GS framebuffer; here we only track which one is "current" / "next" and flip
 * the GS display circuit on the VBlank interrupt, exactly when the N64 VI
 * would have latched the new origin.
 */
#include <ps2/ultra.h>
#include <ps2/ee_shim.h>
#include <ps2/platform.h>

/* Renderer hooks (ps2/src/renderer/gs.c, callable from interrupt context). */
extern void ps2_gs_isr_display_framebuffer(void *n64_fb);
extern void ps2_gs_queue_display_framebuffer(void *n64_fb);
extern void ps2_gs_isr_set_blackout(int black);

static OSMesgQueue *sViEventQueue;
static OSMesg sViEventMsg;
static u32 sViRetraceCount = 1;
static u32 sViRetraceCounter;

static void *volatile sViCurrentFramebuffer;
static void *volatile sViNextFramebuffer;
static volatile s32 sViSwapPending;
static volatile s32 sViBlack = 1;

void ps2_vi_init(void)
{
    sViCurrentFramebuffer = NULL;
    sViNextFramebuffer = NULL;
}

/* Called from the VBlank-start interrupt handler (timing.c). */
void ps2_vi_vblank_isr(void)
{
    if (sViSwapPending)
    {
        sViSwapPending = 0;
        sViCurrentFramebuffer = sViNextFramebuffer;
        ps2_gs_isr_display_framebuffer(sViCurrentFramebuffer);
    }
    if (sViEventQueue != NULL && ++sViRetraceCounter >= sViRetraceCount)
    {
        sViRetraceCounter = 0;
        ps2_osSendMesg_isr(sViEventQueue, sViEventMsg);
    }
}

void osCreateViManager(OSPri pri)
{
    (void)pri;
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount)
{
    s32 intr = ps2_intr_disable();

    sViEventQueue = mq;
    sViEventMsg = msg;
    sViRetraceCount = (retraceCount == 0) ? 1 : retraceCount;
    sViRetraceCounter = 0;
    ps2_intr_restore(intr);
}

void osViSwapBuffer(void *frameBufPtr)
{
    s32 intr = ps2_intr_disable();

    sViNextFramebuffer = frameBufPtr;
    sViSwapPending = 1;
    /* latched by the GS at the next vertical sync; see gs.c */
    ps2_gs_queue_display_framebuffer(frameBufPtr);
    ps2_intr_restore(intr);
}

void *osViGetCurrentFramebuffer(void)
{
    return sViCurrentFramebuffer;
}

void *osViGetNextFramebuffer(void)
{
    return sViSwapPending ? sViNextFramebuffer : sViCurrentFramebuffer;
}

void osViBlack(u8 active)
{
    sViBlack = active;
    ps2_gs_isr_set_blackout(active);
}

/* Mode/feature changes have no GS equivalent that matters: the port always
 * outputs the N64's 320x240 60 Hz picture. */
void osViSetMode(OSViMode *modep)
{
    (void)modep;
}

void osViSetSpecialFeatures(u32 func)
{
    (void)func;
}

void osViSetYScale(f32 scale)
{
    (void)scale;
}

void osViSetXScale(f32 scale)
{
    (void)scale;
}

s32 ps2_vi_is_black(void)
{
    return sViBlack;
}
