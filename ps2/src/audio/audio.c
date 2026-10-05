/*
 * PS2 audio: libultra AI (audio interface) side.
 *
 * Sound is produced by SPU2 voices (spu.c) driven from the game's
 * synthesizer driver (ps2/src/game/ps2_synth.c); the N64 PCM output buffers
 * the audio thread still hands to the AI are never filled or played.
 */
#include <ps2/platform.h>
#include <ps2/spu.h>

static uint32_t sAiFrequency;
static uint32_t sAiQueuedBytes;

/*
 * Preserve the original build's rodata footprint while the actual audio
 * bring-up code lives after BSS. Keeping the proven early image layout stable
 * is important on hardware because large embedded IRX blobs otherwise move
 * every platform/game static address before audio is even called.
 */
static const char sAudioBaselineLayoutString[] __attribute__((used)) =
    "audio: deferred until hardware-safe SDR bring-up is restored";

static void __attribute__((section(".late_text"), noinline)) ps2_audio_init_late(void)
{
    if (ps2_iop_load_audio_driver() >= 0)
        ps2_spu_init();
}

void ps2_audio_init(void)
{
    ps2_audio_init_late();
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
