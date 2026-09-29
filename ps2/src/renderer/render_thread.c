/*
 * Renderer thread: consumes gfx tasks submitted by the game's scheduler.
 *
 * N64: scheduler -> osSpTaskStart -> RSP/RDP run in parallel -> SP/DP
 * interrupts. PS2: scheduler -> osSpTaskStart -> this thread translates the
 * display list into GIF packets (DMA'd while the next chunk is built), waits
 * for the GS, then raises the same two "interrupts" as messages. The thread
 * sits below the game thread in priority, so translation happens while the
 * game waits for its next tick - the same overlap the RCP provided.
 */
#include "render.h"

#include <ps2/input.h>
#include <ps2/platform.h>

#include <kernel.h>
#include <string.h>

#define RENDER_QUEUE 4

typedef struct RenderJob
{
    const void *dl;
    void *cookie;
} RenderJob;

static RenderJob sQueue[RENDER_QUEUE];
static volatile int sHead, sTail;
static int sJobSema = -1;
static int sThreadId = -1;
static uint8_t sStack[64 * 1024] __attribute__((aligned(64)));

/* libultra side (ps2/src/ultra/sp.c): posts OS_EVENT_SP / OS_EVENT_DP. */
extern void ps2_render_task_done(void *cookie);

void ps2_render_enqueue(const void *dl, void *cookie)
{
    int intr = DI();
    int next = (sHead + 1) % RENDER_QUEUE;

    if (next == sTail)
    {
        if (intr)
            EI();
        ps2_panic("render queue overflow");
    }
    sQueue[sHead].dl = dl;
    sQueue[sHead].cookie = cookie;
    sHead = next;
    if (intr)
        EI();
    SignalSema(sJobSema);
}

static void render_one(const RenderJob *job)
{
    uint32_t t0 = ps2_time_us();
    int target;

    memset(&gPS2RenderStats, 0, sizeof(gPS2RenderStats));
    ps2_texcache_frame_begin();

    ps2_gbi_run(job->dl);

    if (ps2_input_overlay_toggle_pressed())
    {
        ps2_overlay_toggle();
    }
    target = ps2_gbi_last_color_target();
    if (target >= 0 && ps2_overlay_enabled())
    {
        ps2_gs_frame_setup(target);
        ps2_overlay_draw();
    }
    ps2_pkt_finish();

    gPS2RenderStats.gfx_us = ps2_time_us() - t0;
    memcpy(&gPS2RenderStatsLast, &gPS2RenderStats, sizeof(gPS2RenderStats));
}

static void render_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        RenderJob job;

        WaitSema(sJobSema);
        job = sQueue[sTail];
        sTail = (sTail + 1) % RENDER_QUEUE;

        render_one(&job);
        ps2_render_task_done(job.cookie);
    }
}

void ps2_render_thread_init(void)
{
    ee_sema_t sema = { 0 };
    ee_thread_t th = { 0 };
    extern void *_gp;

    sema.init_count = 0;
    sema.max_count = RENDER_QUEUE;
    sJobSema = CreateSema(&sema);

    ps2_texcache_init();
    ps2_gbi_init();

    th.func = (void *)render_thread;
    th.stack = sStack;
    th.stack_size = sizeof(sStack);
    th.gp_reg = &_gp;
    th.initial_priority = 80; /* PS2_EE_PRI_RENDER: just below the game thread */
    sThreadId = CreateThread(&th);
    StartThread(sThreadId, NULL);
    ps2_mem_reclassify_static(PS2_MEM_THREADS, sizeof(sStack));
}
