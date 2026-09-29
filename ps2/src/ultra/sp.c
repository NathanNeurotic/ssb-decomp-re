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

void ps2_render_task_done(void *cookie)
{
    (void)cookie;
    ps2_ultra_post_event(OS_EVENT_SP);
    ps2_ultra_post_event(OS_EVENT_DP);
}
