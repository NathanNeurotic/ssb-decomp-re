/*
 * libultra message queues.
 *
 * The OSMesgQueue layout is the game's own (it embeds queues in structs and
 * even on the stack, e.g. func_80000970), so no kernel object is attached to
 * a queue. Blocked threads are linked into the queue's mtqueue / fullqueue
 * lists exactly like libultra does, and parked with SleepThread(); the waker
 * unlinks them and calls WakeupThread(). Wakeup counts that arrive before
 * the waiter actually sleeps are kept by the EE kernel, and every wait is a
 * re-checking loop, so no wakeup can be lost and spurious ones are harmless.
 *
 * Critical sections are short DI/EI pairs; the VBlank interrupt handler uses
 * ps2_osSendMesg_isr().
 */
#include <ps2/ultra.h>
#include <ps2/ee_shim.h>
#include <ps2/platform.h>

/* Insert by priority (highest first, FIFO among equals) like libultra. */
static void waitlist_insert(OSThread **list, OSThread *t)
{
    OSThread **pp = list;

    while (*pp != NULL && (*pp)->priority >= t->priority)
    {
        pp = &(*pp)->next;
    }
    t->next = *pp;
    *pp = t;
    t->queue = list;
}

static OSThread *waitlist_pop(OSThread **list)
{
    OSThread *t = *list;

    if (t != NULL)
    {
        *list = t->next;
        t->next = NULL;
        t->queue = NULL;
    }
    return t;
}

static void waitlist_remove(OSThread **list, OSThread *t)
{
    OSThread **pp;

    for (pp = list; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == t)
        {
            *pp = t->next;
            t->next = NULL;
            t->queue = NULL;
            return;
        }
    }
}

/* Called by osDestroyThread: a destroyed thread must not stay linked. */
void ps2_mesg_forget_thread(OSThread *t)
{
    s32 intr = ps2_intr_disable();

    if (t->queue != NULL)
    {
        waitlist_remove(t->queue, t);
    }
    ps2_intr_restore(intr);
}

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    if (mq == NULL || msg == NULL || count <= 0)
        ps2_panic("osCreateMesgQueue invalid args mq=%p msg=%p count=%ld",
                  mq, msg, (long)count);

    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

static void wake(OSThread *t)
{
    if (t != NULL)
    {
        t->state = OS_STATE_RUNNABLE;
        WakeupThread(PS2_THREAD_EXT(t)->ee_id);
    }
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    OSThread *self = NULL;
    OSThread *waiter;
    s32 intr = ps2_intr_disable();

    while (mq->validCount >= mq->msgCount)
    {
        if (flag != OS_MESG_BLOCK)
        {
            ps2_intr_restore(intr);
            return -1;
        }
        if (self == NULL)
        {
            ps2_intr_restore(intr);
            self = ps2_ultra_self();
            intr = ps2_intr_disable();
            continue;
        }
        waitlist_insert(&mq->fullqueue, self);
        self->state = OS_STATE_WAITING;
        ps2_intr_restore(intr);
        SleepThread();
        intr = ps2_intr_disable();
        if (self->queue != NULL)
        {
            waitlist_remove(self->queue, self);
        }
        self->state = OS_STATE_RUNNING;
    }

    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    waiter = waitlist_pop(&mq->mtqueue);
    ps2_intr_restore(intr);

    wake(waiter);
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    OSThread *self = NULL;
    OSThread *waiter;
    s32 intr = ps2_intr_disable();

    while (mq->validCount >= mq->msgCount)
    {
        if (flag != OS_MESG_BLOCK)
        {
            ps2_intr_restore(intr);
            return -1;
        }
        if (self == NULL)
        {
            ps2_intr_restore(intr);
            self = ps2_ultra_self();
            intr = ps2_intr_disable();
            continue;
        }
        waitlist_insert(&mq->fullqueue, self);
        self->state = OS_STATE_WAITING;
        ps2_intr_restore(intr);
        SleepThread();
        intr = ps2_intr_disable();
        if (self->queue != NULL)
        {
            waitlist_remove(self->queue, self);
        }
        self->state = OS_STATE_RUNNING;
    }

    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    waiter = waitlist_pop(&mq->mtqueue);
    ps2_intr_restore(intr);

    wake(waiter);
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    OSThread *self = NULL;
    OSThread *waiter;
    s32 intr = ps2_intr_disable();

    while (mq->validCount == 0)
    {
        if (flag != OS_MESG_BLOCK)
        {
            ps2_intr_restore(intr);
            return -1;
        }
        if (self == NULL)
        {
            ps2_intr_restore(intr);
            self = ps2_ultra_self();
            intr = ps2_intr_disable();
            continue;
        }
        waitlist_insert(&mq->mtqueue, self);
        self->state = OS_STATE_WAITING;
        ps2_intr_restore(intr);
        SleepThread();
        intr = ps2_intr_disable();
        if (self->queue != NULL)
        {
            waitlist_remove(self->queue, self);
        }
        self->state = OS_STATE_RUNNING;
    }

    if (msg != NULL)
    {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    waiter = waitlist_pop(&mq->fullqueue);
    ps2_intr_restore(intr);

    wake(waiter);
    return 0;
}

/* Interrupt context: interrupts are already disabled, never blocks. */
s32 ps2_osSendMesg_isr(OSMesgQueue *mq, OSMesg msg)
{
    OSThread *waiter;

    if (mq == NULL || mq->validCount >= mq->msgCount)
    {
        return -1;
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    waiter = waitlist_pop(&mq->mtqueue);

    if (waiter != NULL)
    {
        waiter->state = OS_STATE_RUNNABLE;
        iWakeupThread(PS2_THREAD_EXT(waiter)->ee_id);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* osSetEventMesg: hardware events become platform-generated messages. */
/* ------------------------------------------------------------------ */

#define PS2_EVENT_COUNT 23

static struct
{
    OSMesgQueue *mq;
    OSMesg msg;
} sEvents[PS2_EVENT_COUNT];

void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg)
{
    s32 intr;

    if (e >= PS2_EVENT_COUNT)
    {
        return;
    }
    intr = ps2_intr_disable();
    sEvents[e].mq = mq;
    sEvents[e].msg = msg;
    ps2_intr_restore(intr);
}

void ps2_ultra_post_event(OSEvent e)
{
    if (e < PS2_EVENT_COUNT && sEvents[e].mq != NULL)
    {
        osSendMesg(sEvents[e].mq, sEvents[e].msg, OS_MESG_NOBLOCK);
    }
}

void ps2_ultra_post_event_isr(OSEvent e)
{
    if (e < PS2_EVENT_COUNT && sEvents[e].mq != NULL)
    {
        ps2_osSendMesg_isr(sEvents[e].mq, sEvents[e].msg);
    }
}
