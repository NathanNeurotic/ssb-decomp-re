/*
 * PS2 audio backend (SPU2).
 *
 * Bring-up status: output silent. The IOP side (libsd + sdrdrv) is loaded
 * at boot so the SPU2 voice backend can be added without changing the IOP
 * module set; see PS2_PORT.md "Audio design" for the plan (offline
 * VADPCM -> PS-ADPCM conversion, SPU2 voices driven from the game's
 * synthesizer-driver layer).
 */
#include <ps2/platform.h>

static uint32_t sAiFrequency;
static uint32_t sAiQueuedBytes;

void ps2_audio_init(void)
{
    ps2_log("audio: SPU2 backend not active yet (silent)");
}

int32_t ps2_audio_ai_set_frequency(uint32_t frequency)
{
    sAiFrequency = frequency;
    return (int32_t)frequency;
}

int32_t ps2_audio_ai_set_next_buffer(void *buf, uint32_t size)
{
    (void)buf;
    sAiQueuedBytes = size;
    return 0;
}

uint32_t ps2_audio_ai_get_length(void)
{
    /* Report an empty DMA FIFO so the audio thread never stalls on it. */
    return 0;
}

uint32_t ps2_audio_memory_used(void)
{
    return 0;
}
