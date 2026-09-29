/*
 * libultra controller API (osCont*, rumble) on the PS2 pad layer.
 *
 * The N64 SI transaction "start read -> wait for SI interrupt -> get data"
 * becomes "poll pads -> post the completion message immediately". The
 * game's controller thread keeps its original structure and timing (it is
 * driven by the scheduler's retrace clients, not by SI latency).
 */
#include <ps2/ultra.h>
#include <ps2/input.h>
#include <ps2/platform.h>

#include <PR/os.h>

static void si_done(OSMesgQueue *mq)
{
    if (mq != NULL)
    {
        osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    }
}

static void fill_status(OSContStatus *data)
{
    s32 i;

    ps2_input_poll();
    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        const PS2InputState *st = ps2_input_state(i);

        if (st != NULL && st->connected)
        {
            data[i].type = CONT_TYPE_NORMAL;
            /* A "pak" is reported when the pad can rumble, so the game's
             * Rumble Pak logic (osMotorInit) engages. */
            data[i].status = ps2_input_has_rumble(i) ? CONT_CARD_ON : 0;
            data[i].errno = 0;
        }
        else
        {
            data[i].type = 0;
            data[i].status = 0;
            data[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *data)
{
    s32 i;
    u8 bits = 0;

    (void)mq;
    fill_status(data);
    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        if (data[i].errno == 0)
        {
            bits |= (u8)(1 << i);
        }
    }
    *bitpattern = bits;
    return 0;
}

static OSContStatus sQueryStatus[MAXCONTROLLERS];

s32 osContStartQuery(OSMesgQueue *mq)
{
    fill_status(sQueryStatus);
    si_done(mq);
    return 0;
}

void osContGetQuery(OSContStatus *data)
{
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        data[i] = sQueryStatus[i];
    }
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    ps2_input_poll();
    si_done(mq);
    return 0;
}

void osContGetReadData(OSContPad *data)
{
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        const PS2InputState *st = ps2_input_state(i);

        if (st != NULL && st->connected)
        {
            data[i].button = st->buttons;
            data[i].stick_x = st->stick_x;
            data[i].stick_y = st->stick_y;
            data[i].errno = 0;
        }
        else
        {
            data[i].button = 0;
            data[i].stick_x = 0;
            data[i].stick_y = 0;
            data[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContSetCh(u8 ch)
{
    (void)ch;
    return 0;
}

/* Rumble Pak -> DualShock small motor. */
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq;
    pfs->channel = channel;
    pfs->status = 0;
    return ps2_input_has_rumble(channel) ? 0 : PFS_ERR_NOPACK;
}

s32 __osMotorAccess(OSPfs *pfs, s32 start)
{
    ps2_input_set_rumble(pfs->channel, start ? 1 : 0);
    return 0;
}

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern)
{
    (void)mq;
    *pattern = 0; /* no Controller Paks: saves use the PS2 memory card */
    return 0;
}
