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

static void __attribute__((section(".late_text"), noinline)) ps2_audio_init_late(void)
{
    if (ps2_iop_load_audio_driver() >= 0)
        ps2_spu_init();
    else
        ps2_log("audio: deferred until hardware-safe SDR bring-up is restored");
}

void __attribute__((naked, noinline)) ps2_audio_init(void)
{
    /*
     * Exactly three MIPS instructions (12 bytes), matching the proven silent
     * build's wrapper footprint so every following early text symbol keeps its
     * hardware-tested address. The real work is beyond BSS in .late_text.
     */
    __asm__ volatile(
        "nop\n"
        "j ps2_audio_init_late\n"
        "nop\n");
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
