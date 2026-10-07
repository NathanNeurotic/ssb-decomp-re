/*
 * RSP task completion glue.
 *
 * Graphics tasks go to the renderer thread; when it has finished drawing,
 * ps2_render_task_done() raises the two events the game's scheduler waits
 * for, in the RCP's order: SP task done, then DP full sync.
 */
#include <ps2/ultra.h>
#include <ps2/platform.h>

extern void ps2_render_enqueue(const void *dl, void *cookie);

void ps2_gfx_task_submit(OSTask *task)
{
    ps2_render_enqueue(task->t.data_ptr, task);
}

void ps2_render_sp_done(void *cookie)
{
    (void)cookie;
    ps2_ultra_post_event(OS_EVENT_SP);
}

void ps2_render_dp_done(void *cookie)
{
    (void)cookie;
    ps2_ultra_post_event(OS_EVENT_DP);
}

/* Compatibility helper for callers that genuinely complete both stages at
 * once. The threaded renderer uses the split hooks so RSP-style CPU work can
 * overlap the GS finishing the previous packet. */
void ps2_render_task_done(void *cookie)
{
    ps2_render_sp_done(cookie);
    ps2_render_dp_done(cookie);
}
