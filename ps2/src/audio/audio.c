/*
 * PS2 audio: libultra AI (audio interface) side.
 *
 * Sound is produced by SPU2 voices (spu.c) driven from the game's
 * synthesizer driver (ps2/src/game/ps2_synth.c); the N64 PCM output buffers
 * the audio thread still hands to the AI are never filled or played.
 */
#include <ps2/platform.h>
#include <ps2/spu.h>

#include <delaythread.h>
#include <libsdr-common.h>
#include <sifrpc.h>
#include <string.h>

static uint32_t sAiFrequency;
static uint32_t sAiQueuedBytes;

static int sdr_rpc_ready(void)
{
    SifRpcClientData_t probe;
    int i;

    memset(&probe, 0, sizeof(probe));
    for (i = 0; i < 100; i++)
    {
        int rc = sceSifBindRpc(&probe, sce_SDR_DEV, 0);

        if (rc < 0)
        {
            ps2_log("audio: SDR RPC probe failed (%d)", rc);
            return 0;
        }
        if (probe.server != NULL)
            return 1;

        DelayThread(10000); /* bounded to about one second total */
    }
    return 0;
}

void ps2_audio_init(void)
{
    ps2_log("audio: init begin");

    /*
     * ps2sdk's sceSdRemoteInit() waits indefinitely for sce_SDR_DEV. Probe
     * the service first so a bad/missing IOP audio server can never turn
     * sound initialization into another hardware boot hang.
     */
    if (!sdr_rpc_ready())
    {
        ps2_log("audio: SDR RPC absent; loading server");
        if (ps2_iop_load_audio_driver() < 0)
        {
            ps2_log("audio: SDR module failed to start; continuing silent");
            return;
        }
        ps2_log("audio: SDR module load returned; probing RPC");
        if (!sdr_rpc_ready())
        {
            ps2_log("audio: SDR RPC server not ready after bounded wait; continuing silent");
            return;
        }
    }

    ps2_log("audio: SDR RPC ready; initializing SPU2 backend");
    if (ps2_spu_init() < 0)
        ps2_log("audio: SPU2 backend unavailable; continuing silent");
    else
        ps2_log("audio: init complete");
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
