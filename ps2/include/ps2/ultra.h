/*
 * ps2/include/ps2/ultra.h
 *
 * Internal interface of the PS2 libultra replacement (ps2/src/ultra/).
 * Includes the game's own PR/os.h, so it must NOT be combined with ps2sdk
 * headers in the same translation unit (see ee_shim.h).
 */
#ifndef PS2_ULTRA_H
#define PS2_ULTRA_H

#include <PR/os.h>
#include <PR/sptask.h>

/* Per-thread PS2 state, stored in the (otherwise unused) register-context
 * area of the game's OSThread. */
typedef struct PS2ThreadExt
{
    u32 magic;
    s32 ee_id;
    s32 run_sema;       /* osStopThread(self) waits here; osStartThread signals */
    void *stack;
    u32 stack_size;
    u32 stack_class;
    void (*entry)(void *);
    void *arg;
    volatile s32 started;
    volatile s32 suspended; /* stopped by another thread via SuspendThread */
} PS2ThreadExt;

#define PS2_THREAD_MAGIC 0x50533254 /* "PS2T" */
#define PS2_THREAD_EXT(t) ((PS2ThreadExt *)&(t)->context)

/* libultra priority (0..255, higher = more urgent) -> EE priority
 * (1..127, lower = more urgent). Order-preserving for 0..126. */
#define PS2_ULTRA_TO_EE_PRI(p) (((p) >= 127) ? 1 : (127 - (p)))

/* EE priorities of the platform's own threads, placed relative to the
 * game's threads (scheduler 120 -> 7, controller 115 -> 12, audio 110 -> 17,
 * game 50 -> 77, gobj coroutines 51 -> 76, idle 0 -> 127). */
#define PS2_EE_PRI_RENDER 80 /* below the game thread: translate while it waits */
#define PS2_EE_PRI_BOOT   2  /* PS2 main() during boot */

OSThread *ps2_ultra_self(void);
void ps2_ultra_threads_init(void);

/* Interrupt-context variants (never block). */
s32 ps2_osSendMesg_isr(OSMesgQueue *mq, OSMesg msg);
void ps2_ultra_post_event(OSEvent e);
void ps2_ultra_post_event_isr(OSEvent e);

/* Renderer / audio entry points behind osSpTaskStart. */
void ps2_gfx_task_submit(OSTask *task);
void ps2_audio_task_run(OSTask *task);

/* VI state (ps2/src/ultra/vi.c). */
void ps2_vi_init(void);

#endif /* PS2_ULTRA_H */
