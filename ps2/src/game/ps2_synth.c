/*
 * PS2 synthesis driver: n_alAudioFrame() driving SPU2 voices.
 *
 * Everything above the physical-voice layer runs unchanged: the sequence
 * players and the FGM sound engine are called back at their sample times
 * exactly as on the N64, and they queue ALParam updates (start, pitch,
 * volume ramps, pan, FX mix, stop, free) on the physical voices through the
 * n_alSyn* functions. On the N64 n_alAudioFrame() turned those updates into
 * RSP audio commands; here each physical voice is an SPU2 voice and the
 * updates become voice register writes (ps2/src/audio/spu.c). No RSP
 * command list is produced and nothing is mixed on the EE.
 *
 * Differences from the N64 mixer, all small:
 *   - updates inside a frame (1/60 s) take effect at the frame boundary;
 *   - volume ramps are linear, evaluated per frame;
 *   - the aux (wet) send is mixed dry: the game runs with AL_FX_NONE, where
 *     the N64 also mixes the aux bus straight into the main bus.
 */
#include <common.h>
#include <n_audio/n_synthInternals.h>

#include <ps2/spu.h>

extern s16 n_eqpower[];
extern sb32 dSYAudioSoundQuality;

#define N64_OUTPUT_RATE 32000.0F
#define N_EQPOWER_LENGTH 128 /* as in n_env.c */

typedef struct SynVoice
{
    s32 sample;       /* SPU sample id, -1 = none */
    f32 ratio;        /* rs_ratio */
    s32 unity;
    f32 vol;          /* current envelope value, 0..0x7FFF */
    f32 vol_target;
    s32 ramp_left;    /* samples until vol reaches vol_target */
    s32 pan;          /* 0 (left) .. 127 (right) */
    s32 dry, wet;     /* n_eqpower gains */
    s32 active;
} SynVoice;

static SynVoice sVoices[PS2_SPU_VOICES];
static s32 sSpuStarted;

/* _n_timeToSamplesNoRound (static in n_env.c) */
static s32 time_to_samples(s32 micros)
{
    return (s32)(((f32)micros) * n_syn->outputRate / 1000000.0F + 0.5F);
}

static void advance(SynVoice *v, s32 samples)
{
    if (samples <= 0)
    {
        return;
    }
    if (v->ramp_left <= samples)
    {
        v->vol = v->vol_target;
        v->ramp_left = 0;
    }
    else
    {
        v->vol += (v->vol_target - v->vol) * (f32)samples / (f32)v->ramp_left;
        v->ramp_left -= samples;
    }
}

static s32 find_sample(ALWaveTable *wave)
{
    u32 ls = 0, le = 0;

    if (wave == NULL)
    {
        return -1;
    }
    if (wave->type == AL_ADPCM_WAVE)
    {
        ALADPCMloop *loop = wave->waveInfo.adpcmWave.loop;

        if ((loop != NULL) && (loop->count != 0) && (loop->end > loop->start))
        {
            ls = loop->start;
            le = loop->end;
        }
    }
    else if (wave->waveInfo.rawWave.loop != NULL)
    {
        ALRawLoop *loop = wave->waveInfo.rawWave.loop;

        if ((loop->count != 0) && (loop->end > loop->start))
        {
            ls = loop->start;
            le = loop->end;
        }
    }
    return ps2_spu_find_sample((u32)(uintptr_t)wave->base, (u32)wave->len, ls, le);
}

static void start_voice(s32 idx, SynVoice *v, ALWaveTable *wave)
{
    v->sample = find_sample(wave);
    v->active = (v->sample >= 0);
    if (v->active)
    {
        ps2_spu_voice_start(idx, v->sample);
    }
}

static void apply_param(s32 idx, N_PVoice *pv, SynVoice *v, ALParam *p)
{
    switch (p->type)
    {
    case AL_FILTER_START_VOICE_ALT:
    {
        ALStartParamAlt *sp = (ALStartParamAlt *)p;
        s32 volume = ((s32)sp->volume * (s32)sp->volume) >> 15;

        v->unity = (sp->unity != 0);
        v->ratio = sp->pitch;
        v->pan = sp->pan;
        if ((sp->unk1C != 0) || (sp->unk1D != 0x5F))
        {
            v->wet = n_eqpower[N_EQPOWER_LENGTH - sp->unk1C - 1];
            v->dry = n_eqpower[N_EQPOWER_LENGTH - sp->unk1D - 1];
        }
        else
        {
            v->dry = n_eqpower[sp->fxMix];
            v->wet = n_eqpower[N_EQPOWER_LENGTH - sp->fxMix - 1];
        }
        v->vol_target = volume;
        if (sp->samples > 0)
        {
            v->vol = 1.0F;
            v->ramp_left = sp->samples;
        }
        else
        {
            v->vol = volume;
            v->ramp_left = 0;
        }
        start_voice(idx, v, sp->wave);
        break;
    }
    case AL_FILTER_START_VOICE:
    {
        ALStartParam *sp = (ALStartParam *)p;

        if (sp->unity)
        {
            v->unity = TRUE;
        }
        start_voice(idx, v, sp->wave);
        break;
    }
    case AL_FILTER_SET_VOLUME:
    {
        s32 volume = (p->data.i * p->data.i) >> 15;

        v->vol_target = volume;
        v->ramp_left = SAMPLE184(p->moredata.i);
        if (v->ramp_left <= 0)
        {
            v->vol = volume;
        }
        break;
    }
    case AL_FILTER_SET_PAN:
        v->pan = p->data.i;
        break;

    case AL_FILTER_SET_FXAMT:
        v->dry = n_eqpower[p->data.i];
        v->wet = n_eqpower[N_EQPOWER_LENGTH - p->data.i - 1];
        break;

    case AL_FILTER_SET_FXAMT_ALT:
        v->dry = n_eqpower[N_EQPOWER_LENGTH - p->moredata.i];
        v->wet = n_eqpower[N_EQPOWER_LENGTH - p->data.i - 1];
        break;

    case AL_FILTER_SET_PITCH:
        v->ratio = p->data.f;
        break;

    case AL_FILTER_SET_UNITY_PITCH:
        v->unity = TRUE;
        break;

    case AL_FILTER_SET_WAVETABLE:
        v->sample = find_sample((ALWaveTable *)p->data.i);
        break;

    case AL_FILTER_STOP_VOICE:
        ps2_spu_voice_stop(idx);
        v->active = FALSE;
        v->unity = FALSE;
        break;

    case AL_FILTER_FREE_VOICE:
    {
        N_ALFreeParam *fp = (N_ALFreeParam *)p;

        fp->pvoice->offset = 0;
        _n_freePVoice(fp->pvoice);
        break;
    }
    default:
        break;
    }
    (void)pv;
}

static void update_voice(s32 idx, SynVoice *v)
{
    s32 pan = (v->pan < 0) ? 0 : (v->pan >= N_EQPOWER_LENGTH) ? N_EQPOWER_LENGTH - 1 : v->pan;
    f32 mix = (f32)(v->dry + v->wet) * (1.0F / 32768.0F);
    f32 l = v->vol * n_eqpower[pan] * (1.0F / 32768.0F) * mix;
    f32 r = v->vol * n_eqpower[N_EQPOWER_LENGTH - pan - 1] * (1.0F / 32768.0F) * mix;
    f32 ratio = v->unity ? 1.0F : v->ratio;
    f32 pitch = ratio * (N64_OUTPUT_RATE / 48000.0F) * 4096.0F * ps2_spu_sample_pitch_scale(v->sample);

    if (dSYAudioSoundQuality == 0)
    {
        l = r = (l + r) * 0.5F;
    }
    /* SPU2 voice volume: 15-bit, positive half used (0..0x3FFF) */
    l *= 0.5F;
    r *= 0.5F;
    ps2_spu_voice_set_volume(idx, (u16)((l > 16383.0F) ? 16383 : (l < 0.0F) ? 0 : (s32)l),
                             (u16)((r > 16383.0F) ? 16383 : (r < 0.0F) ? 0 : (s32)r));
    ps2_spu_voice_set_pitch(idx, (u16)((pitch > 16383.0F) ? 16383 : (pitch < 1.0F) ? 1 : (s32)pitch));
}

Acmd *n_alAudioFrame(Acmd *cmdList, s32 *cmdLen, s16 *outBuf, s32 outLen)
{
    ALPlayer *client;
    s32 i;

    (void)outBuf;
    *cmdLen = 0;

    if (!sSpuStarted)
    {
        sSpuStarted = TRUE;
        for (i = 0; i < PS2_SPU_VOICES; i++)
        {
            sVoices[i].sample = -1;
        }
    }
    if (n_syn->head == NULL)
    {
        ps2_spu_flush();
        return cmdList;
    }

    /* Call back the players whose next event falls within this frame,
     * exactly like the N64 driver (__n_nextSampleTime). */
    for (;;)
    {
        ALMicroTime delta = 0x7FFFFFFF, temp;

        client = NULL;
        if (n_syn->n_sndp && (temp = n_syn->n_sndp->samplesLeft - n_syn->curSamples) < delta)
        {
            client = n_syn->n_sndp;
            delta = temp;
        }
        if (n_syn->n_seqp1 && (temp = n_syn->n_seqp1->samplesLeft - n_syn->curSamples) < delta)
        {
            client = n_syn->n_seqp1;
            delta = temp;
        }
        if (n_syn->n_seqp2 && (n_syn->n_seqp2->samplesLeft - n_syn->curSamples) < delta)
        {
            client = n_syn->n_seqp2;
        }
        if (client == NULL)
        {
            break;
        }
        n_syn->paramSamples = client->samplesLeft;
        if (n_syn->paramSamples - n_syn->curSamples >= outLen)
        {
            break;
        }
        n_syn->paramSamples &= ~0xF;
        client->samplesLeft += time_to_samples((*client->handler)(client));
    }
    n_syn->paramSamples &= ~0xF;

    /* Apply the queued voice updates that fall within this frame. */
    for (i = 0; i < n_syn->auxBus->sourceCount && i < PS2_SPU_VOICES; i++)
    {
        N_PVoice *pv = n_syn->auxBus->sources[i];
        SynVoice *v = &sVoices[i];
        s32 at = n_syn->curSamples;

        while (pv->em_ctrlList != NULL)
        {
            ALParam *p = pv->em_ctrlList;
            s32 when = p->delta;

            if ((when - n_syn->curSamples) >= outLen)
            {
                break;
            }
            if (when > at)
            {
                advance(v, when - at);
                at = when;
            }
            apply_param(i, pv, v, p);

            pv->em_ctrlList = p->next;
            if (pv->em_ctrlList == NULL)
            {
                pv->em_ctrlTail = NULL;
            }
            _n_freeParam(p);
        }
        advance(v, n_syn->curSamples + outLen - at);
        if (v->active)
        {
            update_voice(i, v);
        }
    }

    if (n_syn->curSamples < 0x7FFFFF47)
    {
        n_syn->curSamples += outLen;
    }
    else
    {
        n_syn->curSamples = 0x80000090;
    }
    _n_collectPVoices();
    ps2_spu_flush();
    return cmdList;
}
