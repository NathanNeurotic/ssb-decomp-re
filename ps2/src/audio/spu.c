/*
 * SPU2 voice backend.
 *
 * The game's synthesizer (ps2/src/game/ps2_synth.c) drives up to 16 voices;
 * they map 1:1 onto voices of SPU2 core 1, which mixes them dry to the
 * output. Samples come from the PS-ADPCM set built offline
 * (ps2/tools/spu_samples.py): the whole set is kept in EE RAM and samples
 * are copied into SPU RAM on first use, with LRU eviction of samples no
 * voice is using.
 *
 * All register changes of one audio frame are collected in a sceSdBatch
 * array, copied to IOP RAM and executed with one sdrdrv call, so a frame
 * costs two IOP round trips regardless of the number of voices.
 */
#include <ps2/platform.h>
#include <ps2/assetpack.h>
#include <ps2/spu.h>

#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <libsdr.h>
#include <sifrpc.h>
#include <string.h>

#define SPU_CORE 1
#define SPU_RAM_FIRST 0x5040u    /* below: core I/O buffers */
#define SPU_RAM_LIMIT 0x1E0000u  /* above: kept free for effect work areas */
#define IOP_STAGE_SIZE (32 * 1024)
#define MAX_BATCH 512
#define MAX_RESIDENT 256

/* Voice envelope: instant attack, full sustain, short linear release so a
 * key-off (the N64 "stop voice") ends the sound within a few ms. */
#define VOICE_ADSR1 SD_SET_ADSR1(SD_ADSR_AR_LINEARi, 0, 0, 0xF)
#define VOICE_ADSR2 SD_SET_ADSR2(SD_ADSR_SR_LINEARi, 0x7F, SD_ADSR_RR_LINEARd, 0x06)

typedef struct Resident
{
    int sample;
    uint32_t addr, size;
    uint32_t last_use;
} Resident;

typedef struct Voice
{
    int sample;          /* sample assigned (kept until reassigned) */
    int start_pending;
    int stop_pending;
    uint16_t voll, volr, pitch;
    uint16_t sent_voll, sent_volr, sent_pitch;
    int playing;
} Voice;

static int sReady;
static uint8_t *sSet;                     /* whole sample region, EE RAM */
static const PS2SpuSampleEntry *sEntries;
static uint32_t sCount;
static int *sResidentOf;                  /* per sample: index into sRes or -1 */
static Resident sRes[MAX_RESIDENT];
static int sResCount;
static uint32_t sClock;
static Voice sVoices[PS2_SPU_VOICES];
static sceSdBatch sBatch[MAX_BATCH] __attribute__((aligned(64)));
static int sBatchCount;
static void *sIopStage;                   /* IOP RAM: sample upload staging */
static void *sIopBatch;                   /* IOP RAM: batch array */
static void *sIopRets;
static PS2SpuStats sStats;

/* libsdr.a (PS2Build's sdr package) references this callback-state block
 * from its callback helpers (sdr_cb.o) without defining it; those helpers
 * are unused here, so plain storage is enough to link. */
unsigned char g_libsdr_cb_data[256] __attribute__((aligned(64)));

static void probe_hardware(void);

/* ------------------------------------------------------------------ */

static void batch_add(uint16_t func, uint16_t entry, uint32_t value)
{
    if (sBatchCount < MAX_BATCH)
    {
        sBatch[sBatchCount].func = func;
        sBatch[sBatchCount].entry = entry;
        sBatch[sBatchCount].value = value;
        sBatchCount++;
    }
}

static void batch_submit(void)
{
    int n = sBatchCount;

    sStats.batch_entries = (uint32_t)n;
    if (n == 0)
        return;
    FlushCache(0);
    sceSdTransToIOP(sBatch, sIopBatch, (uint32_t)((n * (int)sizeof(sceSdBatch) + 63) & ~63), 1);
    sceSdRemote(1, rSdProcBatch, sIopBatch, sIopRets, n);
    sBatchCount = 0;
}

/* Copy [src, src+size) from EE RAM to SPU RAM at addr (blocking). */
static void spu_upload(const uint8_t *src, uint32_t addr, uint32_t size)
{
    uint32_t done = 0;

    FlushCache(0);
    while (done < size)
    {
        uint32_t n = size - done;

        if (n > IOP_STAGE_SIZE)
            n = IOP_STAGE_SIZE;
        sceSdTransToIOP((void *)(src + done), sIopStage, n, 1);
        sceSdRemote(1, rSdVoiceTrans, 0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, sIopStage, addr + done, n);
        sceSdRemote(1, rSdVoiceTransStatus, 0, 1);
        done += n;
    }
    sStats.uploads++;
    sStats.upload_bytes += size;
}

/* ------------------------------------------------------------------ */
/* SPU RAM residency                                                   */
/* ------------------------------------------------------------------ */

static int sample_in_use(int sample)
{
    int v;

    for (v = 0; v < PS2_SPU_VOICES; v++)
        if (sVoices[v].sample == sample)
            return 1;
    return 0;
}

static void res_remove(int i)
{
    sResidentOf[sRes[i].sample] = -1;
    sStats.spu_bytes_used -= sRes[i].size;
    sRes[i] = sRes[--sResCount];
    if (i < sResCount)
        sResidentOf[sRes[i].sample] = i;
}

/* First fit in address order; returns 0 if no gap is large enough. */
static uint32_t find_gap(uint32_t size)
{
    uint32_t cursor = SPU_RAM_FIRST;

    for (;;)
    {
        uint32_t next_end = 0, next_start = SPU_RAM_LIMIT;
        int i, moved = 0;

        /* find the lowest block that overlaps [cursor, cursor+size) */
        for (i = 0; i < sResCount; i++)
        {
            if (sRes[i].addr < cursor + size && sRes[i].addr + sRes[i].size > cursor)
            {
                if (sRes[i].addr + sRes[i].size > next_end)
                    next_end = sRes[i].addr + sRes[i].size;
                moved = 1;
            }
            else if (sRes[i].addr >= cursor && sRes[i].addr < next_start)
            {
                next_start = sRes[i].addr;
            }
        }
        (void)next_start;
        if (!moved)
            return (cursor + size <= SPU_RAM_LIMIT) ? cursor : 0;
        cursor = (next_end + 63) & ~63u;
        if (cursor + size > SPU_RAM_LIMIT)
            return 0;
    }
}

static int make_resident(int sample)
{
    const PS2SpuSampleEntry *e = &sEntries[sample];
    uint32_t addr;

    if (sResidentOf[sample] >= 0)
    {
        sRes[sResidentOf[sample]].last_use = sClock;
        return 1;
    }
    for (;;)
    {
        int i, lru = -1;

        addr = (sResCount < MAX_RESIDENT) ? find_gap(e->data_size) : 0;
        if (addr != 0)
            break;
        for (i = 0; i < sResCount; i++)
        {
            if (!sample_in_use(sRes[i].sample) && (lru < 0 || sRes[i].last_use < sRes[lru].last_use))
                lru = i;
        }
        if (lru < 0)
            return 0; /* everything resident is playing */
        res_remove(lru);
        sStats.evictions++;
    }
    spu_upload(sSet + e->data_off, addr, e->data_size);
    sRes[sResCount].sample = sample;
    sRes[sResCount].addr = addr;
    sRes[sResCount].size = e->data_size;
    sRes[sResCount].last_use = sClock;
    sResidentOf[sample] = sResCount++;
    sStats.spu_bytes_used += e->data_size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

int ps2_spu_init(void)
{
    PS2SpuSampleHeader head;
    uint32_t size, i;
    int v;

    ps2_rom_read(PS2_SPU_SAMPLES_VROM, &head, sizeof(head));
    if (memcmp(head.magic, PS2_SPU_SAMPLES_MAGIC, 4) != 0 || head.version != PS2_SPU_SAMPLES_VERSION)
    {
        ps2_log("audio: no SPU sample set in the asset pack (rebuild it); audio stays silent");
        return -1;
    }
    /* size = end of the last sample's data */
    {
        PS2SpuSampleEntry last;

        ps2_rom_read(PS2_SPU_SAMPLES_VROM + head.entries_offset + (head.count - 1) * sizeof(last), &last,
                     sizeof(last));
        size = last.data_off + last.data_size;
    }
    sSet = (uint8_t *)ps2_mem_alloc(PS2_MEM_AUDIO, size, 64);
    sResidentOf = (int *)ps2_mem_alloc(PS2_MEM_AUDIO, head.count * sizeof(int), 16);
    if (sSet == NULL || sResidentOf == NULL)
    {
        ps2_log("audio: cannot allocate %u KiB for the sample set; audio stays silent", (unsigned)(size / 1024));
        return -1;
    }
    ps2_rom_read(PS2_SPU_SAMPLES_VROM, sSet, size);
    sEntries = (const PS2SpuSampleEntry *)(sSet + head.entries_offset);
    sCount = head.count;
    for (i = 0; i < sCount; i++)
        sResidentOf[i] = -1;

    if (SifSearchModuleByName("sdr_driver") < 0)
    {
        ps2_log("audio: sdrdrv unavailable; audio stays silent");
        return -1;
    }

    SifInitIopHeap();
    sIopStage = SifAllocIopHeap(IOP_STAGE_SIZE);
    sIopBatch = SifAllocIopHeap(MAX_BATCH * sizeof(sceSdBatch));
    sIopRets = SifAllocIopHeap(MAX_BATCH * sizeof(uint32_t));
    if (sIopStage == NULL || sIopBatch == NULL || sIopRets == NULL)
    {
        ps2_log("audio: IOP heap allocation failed; audio stays silent");
        return -1;
    }
    if (sceSdRemoteInit() < 0)
    {
        ps2_log("audio: sdrdrv RPC unavailable; audio stays silent");
        return -1;
    }
    sceSdRemote(1, rSdInit, 0);

    /* core 1: all voices dry to the output, master volume full, no effects */
    batch_add(SD_BATCH_SETCORE, SPU_CORE | SD_CORE_EFFECT_ENABLE, 0);
    batch_add(SD_BATCH_SETPARAM, SPU_CORE | SD_PARAM_MMIX, 0xCCC);
    batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_VMIXL, 0xFFFFFF);
    batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_VMIXR, 0xFFFFFF);
    batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_VMIXEL, 0);
    batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_VMIXER, 0);
    batch_add(SD_BATCH_SETPARAM, SPU_CORE | SD_PARAM_MVOLL, 0x3FFF);
    batch_add(SD_BATCH_SETPARAM, SPU_CORE | SD_PARAM_MVOLR, 0x3FFF);
    batch_add(SD_BATCH_SETPARAM, 0 | SD_PARAM_MVOLL, 0x3FFF);
    batch_add(SD_BATCH_SETPARAM, 0 | SD_PARAM_MVOLR, 0x3FFF);
    for (v = 0; v < PS2_SPU_VOICES; v++)
    {
        batch_add(SD_BATCH_SETPARAM, SD_VOICE(SPU_CORE, v) | SD_VPARAM_VOLL, 0);
        batch_add(SD_BATCH_SETPARAM, SD_VOICE(SPU_CORE, v) | SD_VPARAM_VOLR, 0);
        batch_add(SD_BATCH_SETPARAM, SD_VOICE(SPU_CORE, v) | SD_VPARAM_ADSR1, VOICE_ADSR1);
        batch_add(SD_BATCH_SETPARAM, SD_VOICE(SPU_CORE, v) | SD_VPARAM_ADSR2, VOICE_ADSR2);
        sVoices[v].sample = -1;
    }
    batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_KOFF, 0xFFFFFF);
    batch_submit();

    sStats.samples = sCount;
    sStats.spu_bytes_total = SPU_RAM_LIMIT - SPU_RAM_FIRST;
    sReady = 1;
    ps2_log("audio: SPU2 backend on core %d, %u samples (%u KiB PS-ADPCM in EE RAM), %u KiB SPU cache", SPU_CORE,
            (unsigned)sCount, (unsigned)(size / 1024), (unsigned)(sStats.spu_bytes_total / 1024));
    return 0;
}

int ps2_spu_ready(void)
{
    return sReady;
}

int ps2_spu_find_sample(uint32_t rom_key, uint32_t len, uint32_t loop_start, uint32_t loop_end)
{
    int lo = 0, hi = (int)sCount - 1, i;

    if (!sReady)
        return -1;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (sEntries[mid].key < rom_key || (sEntries[mid].key == rom_key && sEntries[mid].len < len))
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < (int)sCount && sEntries[i].key == rom_key && sEntries[i].len == len; i++)
    {
        if (sEntries[i].loop_start == loop_start && sEntries[i].loop_end == loop_end)
            return i;
    }
    /* same data, different loop points than converted: use the first one */
    if (lo < (int)sCount && sEntries[lo].key == rom_key && sEntries[lo].len == len)
        return lo;
    sStats.missing++;
    return -1;
}

float ps2_spu_sample_pitch_scale(int sample)
{
    return (sample >= 0 && (uint32_t)sample < sCount) ? sEntries[sample].pitch_scale : 1.0f;
}

void ps2_spu_voice_start(int voice, int sample)
{
    if (!sReady || voice < 0 || voice >= PS2_SPU_VOICES || sample < 0)
        return;
    sVoices[voice].sample = sample;
    sVoices[voice].start_pending = 1;
    sVoices[voice].stop_pending = 0;
}

void ps2_spu_voice_stop(int voice)
{
    if (!sReady || voice < 0 || voice >= PS2_SPU_VOICES)
        return;
    sVoices[voice].start_pending = 0;
    if (sVoices[voice].playing)
        sVoices[voice].stop_pending = 1;
}

void ps2_spu_voice_set_volume(int voice, uint16_t left, uint16_t right)
{
    if (voice >= 0 && voice < PS2_SPU_VOICES)
    {
        sVoices[voice].voll = left;
        sVoices[voice].volr = right;
    }
}

void ps2_spu_voice_set_pitch(int voice, uint16_t pitch)
{
    if (voice >= 0 && voice < PS2_SPU_VOICES)
        sVoices[voice].pitch = pitch;
}

void ps2_spu_flush(void)
{
    uint32_t kon = 0, koff = 0;
    int v, active = 0;

    if (!sReady)
        return;
    sClock++;
    for (v = 0; v < PS2_SPU_VOICES; v++)
    {
        Voice *vo = &sVoices[v];
        uint16_t e = SD_VOICE(SPU_CORE, v);

        if (vo->stop_pending)
        {
            /* The N64 stops a voice instantly: key off and mute it, rather
             * than leaving it to the SPU2 release envelope. */
            koff |= 1u << v;
            vo->stop_pending = 0;
            vo->playing = 0;
            vo->voll = vo->volr = 0;
        }
        if (vo->start_pending)
        {
            vo->start_pending = 0;
            if (make_resident(vo->sample))
            {
                const Resident *r = &sRes[sResidentOf[vo->sample]];

                batch_add(SD_BATCH_SETADDR, e | SD_VADDR_SSA, r->addr);
                /* force the voice parameters out with the key-on */
                vo->sent_voll = (uint16_t)~vo->voll;
                vo->sent_volr = (uint16_t)~vo->volr;
                vo->sent_pitch = (uint16_t)~vo->pitch;
                kon |= 1u << v;
                koff &= ~(1u << v);
                vo->playing = 1;
            }
        }
        if (vo->voll != vo->sent_voll)
        {
            batch_add(SD_BATCH_SETPARAM, e | SD_VPARAM_VOLL, vo->voll);
            vo->sent_voll = vo->voll;
        }
        if (vo->volr != vo->sent_volr)
        {
            batch_add(SD_BATCH_SETPARAM, e | SD_VPARAM_VOLR, vo->volr);
            vo->sent_volr = vo->volr;
        }
        if (vo->pitch != vo->sent_pitch)
        {
            batch_add(SD_BATCH_SETPARAM, e | SD_VPARAM_PITCH, vo->pitch);
            vo->sent_pitch = vo->pitch;
        }
        active += vo->playing;
    }
    if (koff)
        batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_KOFF, koff);
    if (kon)
        batch_add(SD_BATCH_SETSWITCH, SPU_CORE | SD_SWITCH_KON, kon);
    sStats.active_voices = (uint32_t)active;
    sStats.resident = (uint32_t)sResCount;
    batch_submit();
    if ((sClock % 300) == 0)
        probe_hardware();
}

const PS2SpuStats *ps2_spu_stats(void)
{
    return &sStats;
}

/* Health check, run from the audio thread every few seconds: how many
 * voices have a non-zero hardware envelope, and a checksum of their play
 * addresses (it changes between probes when the SPU2 is really playing). */
static void probe_hardware(void)
{
    uint32_t sounding = 0, nax_sum = 0;
    int v;

    for (v = 0; v < PS2_SPU_VOICES; v++)
    {
        if ((uint32_t)sceSdRemote(1, rSdGetParam, SD_VOICE(SPU_CORE, v) | SD_VPARAM_ENVX) & 0x7FFF)
        {
            sounding++;
            nax_sum += (uint32_t)sceSdRemote(1, rSdGetAddr, SD_VOICE(SPU_CORE, v) | SD_VADDR_NAX);
        }
    }
    sStats.hw_sounding = sounding;
    sStats.hw_nax_sum = nax_sum;
}
