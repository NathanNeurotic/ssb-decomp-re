/*
 * Debug overlay (SELECT + R3 toggles). Compiled out when PS2_DEBUG_OVERLAY=0.
 *
 * Shows what matters for staying inside the PS2's limits: frame rate and
 * frame cost, EE RAM by category against the 24 MiB budget, arena and
 * texture-cache use, GS VRAM, upload traffic and primitive counts.
 */
#include "render.h"

#include <ps2/platform.h>

#include <stdio.h>
#include <string.h>

#if PS2_DEBUG_OVERLAY

static int sEnabled;
static volatile uint32_t sPresented;
static uint32_t sFpsWindowStart;
static uint32_t sFpsWindowFrames;
static uint32_t sFpsX10;
static uint32_t sIoWindowUs, sIoMsPerSecond;
static char sSceneName[32] = "?";

extern uint32_t ps2_arena_size(void);
extern uint32_t ps2_audio_memory_used(void);
extern int32_t ps2_ultra_coroutine_stacks_in_use(void);
extern uint32_t ps2_assets_bytes_read(void);
extern uint32_t ps2_assets_io_time_us(void);
/* Game heap pointer (src/sys/taskman.c) - sampled for arena use. */
extern uint8_t gPS2SceneArena[];
typedef struct { uint32_t id; void *start, *end, *ptr; } PS2GameHeapView;
extern PS2GameHeapView gSYTaskmanGeneralHeap __attribute__((weak));

int ps2_overlay_enabled(void)
{
    return sEnabled;
}

void ps2_overlay_toggle(void)
{
    sEnabled = !sEnabled;
    sFpsWindowStart = ps2_time_us();
    sFpsWindowFrames = sPresented;
    sIoWindowUs = ps2_assets_io_time_us();
    sIoMsPerSecond = 0;
}

void ps2_overlay_set_scene_name(const char *name)
{
    strncpy(sSceneName, name, sizeof(sSceneName) - 1);
}

/* Interrupt context (VBlank flip). */
void ps2_overlay_frame_presented(void)
{
    sPresented++;
}

static void line(int *y, uint32_t color, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#include <stdarg.h>

static void line(int *y, uint32_t color, const char *fmt, ...)
{
    char buf[48];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ps2_gs_text(4, *y, color, buf);
    *y += 9;
}

void ps2_overlay_draw(void)
{
    const PS2MemStats *m = ps2_mem_stats();
    const PS2TexStats *t = ps2_texcache_stats();
    const PS2RenderStats *r = &gPS2RenderStatsLast;
    uint32_t now = ps2_time_us();
    uint32_t arena_used = 0;
    int y = 12; /* clear of the top overscan area on TVs */
    uint32_t warn = 0x4040FF, ok = 0xE0FFE0, dim = 0xC0C0C0;

    if (now - sFpsWindowStart >= 1000000u)
    {
        uint32_t frames = sPresented - sFpsWindowFrames;
        uint32_t io = ps2_assets_io_time_us();

        sIoMsPerSecond = (uint32_t)(((uint64_t)(io - sIoWindowUs) * 1000u) /
                                  (now - sFpsWindowStart));
        sIoWindowUs = io;
        sFpsX10 = (frames * 10000000u) / (now - sFpsWindowStart);
        sFpsWindowStart = now;
        sFpsWindowFrames = sPresented;
    }
    if (&gSYTaskmanGeneralHeap != NULL && gSYTaskmanGeneralHeap.ptr != NULL)
    {
        arena_used = (uint32_t)((uint8_t *)gSYTaskmanGeneralHeap.ptr - gPS2SceneArena);
    }

    ps2_gs_rect(0, 0, 200, 150, 0xA0000000);
    line(&y, ok, "FPS %u.%u  gfx %u.%ums", (unsigned)(sFpsX10 / 10), (unsigned)(sFpsX10 % 10),
         (unsigned)(r->gfx_us / 1000), (unsigned)((r->gfx_us / 100) % 10));
    line(&y, (m->total_used > m->budget) ? warn : ok, "EE %uK/%uK pk %uK", (unsigned)(m->total_used >> 10),
         (unsigned)(m->budget >> 10), (unsigned)(m->total_peak >> 10));
    line(&y, dim, " code %uK arena %uK/%uK", (unsigned)(m->used[PS2_MEM_CODE_STATIC] >> 10),
         (unsigned)(arena_used >> 10), (unsigned)(ps2_arena_size() >> 10));
    line(&y, dim, " gfx %uK tex %uK thr %uK", (unsigned)(m->reserved[PS2_MEM_GFX_STAGING] >> 10),
         (unsigned)(t->ee_bytes >> 10), (unsigned)((m->reserved[PS2_MEM_THREADS] + m->used[PS2_MEM_THREADS]) >> 10));
    line(&y, dim, " audio %uK co-stk %d", (unsigned)(ps2_audio_memory_used() >> 10),
         (int)ps2_ultra_coroutine_stacks_in_use());
    line(&y, ok, "VRAM tex %uK/%uK pk %uK", (unsigned)(t->vram_blocks_used / 4),
         (unsigned)(PS2_TEX_POOL_BLOCKS / 4), (unsigned)(t->vram_blocks_peak / 4));
    line(&y, dim, "tex %u ent, up %u (%uK) cv %u", (unsigned)t->entries, (unsigned)r->tex_uploads,
         (unsigned)(r->tex_upload_bytes >> 10), (unsigned)r->tex_converts);
    line(&y, dim, "tri %u rect %u batch %u", (unsigned)r->triangles, (unsigned)r->rects, (unsigned)r->batches);
    line(&y, dim, "dl %u st %u pkt %uK", (unsigned)r->dl_commands, (unsigned)r->state_writes,
         (unsigned)(r->packet_bytes >> 10));
    line(&y, dim, "io %uK wait %ums/s %s", (unsigned)(ps2_assets_bytes_read() >> 10),
         (unsigned)sIoMsPerSecond, sSceneName);
    if (r->unknown_cmds)
    {
        line(&y, warn, "unknown GBI cmds %u", (unsigned)r->unknown_cmds);
    }
}

#else /* !PS2_DEBUG_OVERLAY */

int ps2_overlay_enabled(void) { return 0; }
void ps2_overlay_toggle(void) {}
void ps2_overlay_draw(void) {}
void ps2_overlay_frame_presented(void) {}
void ps2_overlay_set_scene_name(const char *name) { (void)name; }

#endif
