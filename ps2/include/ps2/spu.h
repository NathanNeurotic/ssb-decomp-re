/*
 * ps2/include/ps2/spu.h - SPU2 voice backend used by the game's synthesizer
 * driver replacement (ps2/src/game/ps2_synth.c).
 *
 * Only <stdint.h> types, so it can be included from game code (N64 types)
 * and platform code (ps2sdk types) alike.
 */
#ifndef PS2_SPU_H
#define PS2_SPU_H

#include <stdint.h>

#define PS2_SPU_VOICES 24 /* voices of one SPU2 core (the game uses 16) */

/* Brings up libsd/sdrdrv, loads the sample set into EE RAM and configures
 * core 1 (dry output). Returns 0 on success; the game then runs silent. */
int ps2_spu_init(void);
int ps2_spu_ready(void);
void ps2_spu_shutdown(void);

/* Finds the converted sample for an N64 wavetable (wav->base, wav->len and
 * its loop points in samples, 0/0 when not looped). Returns an id or -1. */
int ps2_spu_find_sample(uint32_t rom_key, uint32_t len, uint32_t loop_start, uint32_t loop_end);
float ps2_spu_sample_pitch_scale(int sample);

/* Voice control. Changes are queued and sent to the SPU2 in one batch by
 * ps2_spu_flush(), which the synthesizer calls once per audio frame.
 *   volume: 0..0x3FFF per side; pitch: SPU2 units (0x1000 = 48 kHz). */
void ps2_spu_voice_start(int voice, int sample);
void ps2_spu_voice_stop(int voice);
void ps2_spu_voice_set_volume(int voice, uint16_t left, uint16_t right);
void ps2_spu_voice_set_pitch(int voice, uint16_t pitch);
void ps2_spu_flush(void);

typedef struct PS2SpuStats
{
    uint32_t samples;          /* in the sample set */
    uint32_t resident;         /* currently in SPU RAM */
    uint32_t spu_bytes_used;
    uint32_t spu_bytes_total;
    uint32_t uploads;
    uint32_t upload_bytes;
    uint32_t evictions;
    uint32_t missing;          /* voice starts with no converted sample */
    uint32_t active_voices;
    uint32_t batch_entries;    /* last flush */
    uint32_t hw_sounding;      /* voices with a non-zero SPU2 envelope (probed every 300 frames) */
    uint32_t hw_nax_sum;       /* checksum of their play addresses */
} PS2SpuStats;

const PS2SpuStats *ps2_spu_stats(void);

#endif /* PS2_SPU_H */
