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
#include <ps2/spu.h>

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
static uint32_t sTasksDone;
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

    if (sTasksDone == 0)
    {
        /* The N64 game clears its framebuffers with CPU stores to RDRAM
         * (scmanager boot, staff roll, congratulations); here the first
         * game frame starts from cleared GS framebuffers instead. */
        int i;

        for (i = 0; i < PS2_FB_COUNT; i++)
        {
            ps2_gs_clear(i, 0x000000, i == 0);
        }
    }

    {
        extern int gPS2GbiTrace;

        (void)gPS2GbiTrace; /* debug: set > 0 to log the next N GBI commands */
    }
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

    sTasksDone++;
    if (sTasksDone <= 3 || (sTasksDone % 600) == 0)
    {
        ps2_log("gfx #%u: fb %d, %u cmds, %u tris, %u rects, %u uploads, %u unk, %u us", (unsigned)sTasksDone, target,
                (unsigned)gPS2RenderStats.dl_commands, (unsigned)gPS2RenderStats.triangles,
                (unsigned)gPS2RenderStats.rects, (unsigned)gPS2RenderStats.tex_uploads,
                (unsigned)gPS2RenderStats.unknown_cmds, (unsigned)gPS2RenderStats.gfx_us);
        if (sTasksDone > 3)
        {
            const PS2MemStats *ms = ps2_mem_stats();
            const PS2TexStats *ts = ps2_texcache_stats();

            ps2_log("mem: %u KiB committed, peak %u KiB; tex: %u entries, EE %u/%u KiB, VRAM %u/%u blocks peak, %u evictions",
                    (unsigned)(ms->total_used / 1024), (unsigned)(ms->total_peak / 1024), (unsigned)ts->entries,
                    (unsigned)(ts->ee_bytes / 1024), (unsigned)(ts->ee_capacity / 1024),
                    (unsigned)ts->vram_blocks_peak, (unsigned)PS2_TEX_POOL_BLOCKS, (unsigned)ts->evictions);
            {
                const PS2SpuStats *ss = ps2_spu_stats();

                ps2_log("spu: %u voices, %u/%u samples resident (%u/%u KiB), %u uploads (%u KiB), %u evictions, "
                        "%u missing, batch %u, hw %u sounding (nax %x)",
                        (unsigned)ss->active_voices, (unsigned)ss->resident, (unsigned)ss->samples,
                        (unsigned)(ss->spu_bytes_used / 1024), (unsigned)(ss->spu_bytes_total / 1024),
                        (unsigned)ss->uploads, (unsigned)(ss->upload_bytes / 1024), (unsigned)ss->evictions,
                        (unsigned)ss->missing, (unsigned)ss->batch_entries, (unsigned)ss->hw_sounding,
                        (unsigned)ss->hw_nax_sum);
            }
        }
    }
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
