/*
 * Audio RSP tasks.
 *
 * The N64 synthesizer builds an audio command list each frame for the RSP
 * audio microcode. The PS2 port does not run that microcode; the native SPU2
 * backend (ps2/src/audio/) will instead take over at the synthesizer-driver
 * level. Until then, audio tasks complete immediately (silence), which keeps
 * the game's audio thread, sequence players and timing running unchanged.
 */
#include <ps2/ultra.h>

void ps2_audio_task_run(OSTask *task)
{
    (void)task;
    ps2_ultra_post_event(OS_EVENT_SP);
}
