/*
 * libultra threads on EE kernel threads.
 *
 * Both kernels are strictly priority-preemptive without time slicing, so the
 * game's threading model maps directly:
 *
 *   osCreateThread   -> CreateThread (dormant) + a run semaphore
 *   osStartThread    -> StartThread first time, then SignalSema(run)
 *   osStopThread(0)  -> WaitSema(run)   (gobj coroutines park here)
 *   osDestroyThread  -> TerminateThread + DeleteThread
 *
 * N64 thread stacks were sized for IDO code (the gobj coroutines get 1.5 KiB).
 * The EE threads therefore run on stacks from the platform's own pools; the
 * game's stack arrays stay untouched, which also keeps its stack-canary checks
 * valid. Stack class is chosen from the libultra thread id: the object
 * manager's coroutines use ids >= 10000000, system threads use small ids.
 */
#include <ps2/ultra.h>
#include <ps2/ee_shim.h>
#include <ps2/platform.h>


_Static_assert(sizeof(__OSThreadContext) >= sizeof(PS2ThreadExt), "OSThread context too small for PS2ThreadExt");

#define STACK_CLASS_SYSTEM 0
#define STACK_CLASS_COROUTINE 1

#define SYSTEM_STACK_SIZE (32 * 1024)
#define COROUTINE_STACK_SIZE (16 * 1024)
#define COROUTINE_STACK_SLOTS 48

#define EE_MAX_THREADS 256

/* Coroutine stacks: one fixed slab, recycled through a free list. */
static u8 sCoroutineStacks[COROUTINE_STACK_SLOTS][COROUTINE_STACK_SIZE] __attribute__((aligned(64)));
static s32 sCoroutineFree[COROUTINE_STACK_SLOTS];
static s32 sCoroutineFreeCount;
static s32 sCoroutineInUse;
static s32 sCoroutinePeak;

/* EE thread id -> OSThread (for ps2_ultra_self()). */
static OSThread *sThreadMap[EE_MAX_THREADS];

/* OSThreads for EE threads the game did not create (PS2 main, renderer...). */
#define FOREIGN_THREADS 8
static OSThread sForeign[FOREIGN_THREADS];
static s32 sForeignCount;

static OSThread *sAllThreads; /* tlnext list, for debugging */

void ps2_ultra_threads_init(void)
{
    s32 i;

    for (i = 0; i < COROUTINE_STACK_SLOTS; i++)
    {
        sCoroutineFree[i] = COROUTINE_STACK_SLOTS - 1 - i;
    }
    sCoroutineFreeCount = COROUTINE_STACK_SLOTS;
    ps2_mem_reclassify_static(PS2_MEM_THREADS, sizeof(sCoroutineStacks));
}

static void *stack_alloc(u32 cls, u32 *out_size)
{
    if (cls == STACK_CLASS_COROUTINE)
    {
        s32 intr = ps2_intr_disable();

        if (sCoroutineFreeCount > 0)
        {
            s32 slot = sCoroutineFree[--sCoroutineFreeCount];

            sCoroutineInUse++;
            if (sCoroutineInUse > sCoroutinePeak)
            {
                sCoroutinePeak = sCoroutineInUse;
            }
            ps2_intr_restore(intr);
            *out_size = COROUTINE_STACK_SIZE;
            return sCoroutineStacks[slot];
        }
        ps2_intr_restore(intr);
        ps2_log("threads: coroutine stack pool exhausted (%d), using heap", COROUTINE_STACK_SLOTS);
    }
    *out_size = (cls == STACK_CLASS_COROUTINE) ? COROUTINE_STACK_SIZE : SYSTEM_STACK_SIZE;
    return ps2_mem_alloc(PS2_MEM_THREADS, *out_size, 64);
}

static void stack_free(void *stack, u32 size)
{
    u8 *p = (u8 *)stack;

    if (p >= &sCoroutineStacks[0][0] &&
        p < (&sCoroutineStacks[0][0] + sizeof(sCoroutineStacks)))
    {
        s32 intr = ps2_intr_disable();

        sCoroutineFree[sCoroutineFreeCount++] = (s32)((p - &sCoroutineStacks[0][0]) / COROUTINE_STACK_SIZE);
        sCoroutineInUse--;
        ps2_intr_restore(intr);
    }
    else
    {
        ps2_mem_free(PS2_MEM_THREADS, stack, size);
    }
}

static s32 create_run_sema(void)
{
    ps2_ee_sema_t sema;

    __builtin_memset(&sema, 0, sizeof(sema));
    sema.init_count = 0;
    sema.max_count = 1;
    return CreateSema(&sema);
}

static void thread_trampoline(void *arg)
{
    OSThread *t = (OSThread *)arg;
    PS2ThreadExt *ext = PS2_THREAD_EXT(t);

    ext->entry(ext->arg);

    /* libultra threads never return; treat it like osDestroyThread(NULL). */
    osDestroyThread(NULL);
}

OSThread *ps2_ultra_self(void)
{
    s32 id = GetThreadId();
    OSThread *t;

    if (id >= 0 && id < EE_MAX_THREADS && sThreadMap[id] != NULL)
    {
        return sThreadMap[id];
    }

    /* A platform thread touching libultra: give it an OSThread. */
    {
        s32 intr = ps2_intr_disable();

        if (sForeignCount >= FOREIGN_THREADS)
        {
            ps2_intr_restore(intr);
            ps2_panic("too many foreign threads using libultra");
        }
        t = &sForeign[sForeignCount++];
        ps2_intr_restore(intr);
    }
    __builtin_memset(t, 0, sizeof(*t));
    t->priority = OS_PRIORITY_APPMAX;
    t->state = OS_STATE_RUNNING;
    t->id = -1;
    PS2_THREAD_EXT(t)->magic = PS2_THREAD_MAGIC;
    PS2_THREAD_EXT(t)->ee_id = id;
    PS2_THREAD_EXT(t)->run_sema = create_run_sema();
    PS2_THREAD_EXT(t)->started = 1;
    if (id >= 0 && id < EE_MAX_THREADS)
    {
        sThreadMap[id] = t;
    }
    return t;
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri)
{
    PS2ThreadExt *ext = PS2_THREAD_EXT(t);
    ps2_ee_thread_t th;
    u32 cls = (id >= 1000) ? STACK_CLASS_COROUTINE : STACK_CLASS_SYSTEM;

    (void)sp; /* see file comment: EE threads use platform-owned stacks */

    __builtin_memset(t, 0, sizeof(*t));
    t->priority = pri;
    t->id = id;
    t->state = OS_STATE_STOPPED;

    ext->magic = PS2_THREAD_MAGIC;
    ext->entry = entry;
    ext->arg = arg;
    ext->stack_class = cls;
    ext->stack = stack_alloc(cls, &ext->stack_size);
    if (ext->stack == NULL)
    {
        ps2_panic("osCreateThread(id=%ld) cannot allocate %lu-byte stack",
                  (long)id, (unsigned long)ext->stack_size);
    }
    ext->run_sema = create_run_sema();

    __builtin_memset(&th, 0, sizeof(th));
    th.func = (void *)thread_trampoline;
    th.stack = ext->stack;
    th.stack_size = (int)ext->stack_size;
    th.gp_reg = &_gp;
    th.initial_priority = PS2_ULTRA_TO_EE_PRI(pri);
    ext->ee_id = CreateThread(&th);

    if (ext->ee_id < 0 || ext->run_sema < 0)
    {
        ps2_panic("osCreateThread(id=%ld) failed: ee=%ld sema=%ld", (long)id, (long)ext->ee_id, (long)ext->run_sema);
    }
    if (ext->ee_id < EE_MAX_THREADS)
    {
        sThreadMap[ext->ee_id] = t;
    }

    {
        s32 intr = ps2_intr_disable();

        t->tlnext = sAllThreads;
        sAllThreads = t;
        ps2_intr_restore(intr);
    }
}

void osStartThread(OSThread *t)
{
    PS2ThreadExt *ext = PS2_THREAD_EXT(t);

    if (!ext->started)
    {
        ext->started = 1;
        t->state = OS_STATE_RUNNABLE;
        StartThread(ext->ee_id, t);
    }
    else if (ext->suspended)
    {
        ext->suspended = 0;
        t->state = OS_STATE_RUNNABLE;
        ResumeThread(ext->ee_id);
    }
    else if (t->state == OS_STATE_STOPPED)
    {
        t->state = OS_STATE_RUNNABLE;
        SignalSema(ext->run_sema);
    }
}

void osStopThread(OSThread *t)
{
    OSThread *self = ps2_ultra_self();

    if (t == NULL || t == self)
    {
        self->state = OS_STATE_STOPPED;
        WaitSema(PS2_THREAD_EXT(self)->run_sema);
        self->state = OS_STATE_RUNNING;
    }
    else if (PS2_THREAD_EXT(t)->started && t->state != OS_STATE_STOPPED)
    {
        PS2_THREAD_EXT(t)->suspended = 1;
        t->state = OS_STATE_STOPPED;
        SuspendThread(PS2_THREAD_EXT(t)->ee_id);
    }
}

static void unlink_thread(OSThread *t)
{
    OSThread **pp;
    s32 intr = ps2_intr_disable();

    for (pp = &sAllThreads; *pp != NULL; pp = &(*pp)->tlnext)
    {
        if (*pp == t)
        {
            *pp = t->tlnext;
            break;
        }
    }
    if (PS2_THREAD_EXT(t)->ee_id >= 0 && PS2_THREAD_EXT(t)->ee_id < EE_MAX_THREADS)
    {
        sThreadMap[PS2_THREAD_EXT(t)->ee_id] = NULL;
    }
    ps2_intr_restore(intr);
}

extern void ps2_mesg_forget_thread(OSThread *t);

void osDestroyThread(OSThread *t)
{
    OSThread *self = ps2_ultra_self();
    PS2ThreadExt *ext;

    if (t == NULL)
    {
        t = self;
    }
    ext = PS2_THREAD_EXT(t);
    ps2_mesg_forget_thread(t);
    unlink_thread(t);
    t->state = 0;

    if (t == self)
    {
        /* The stack slot is recycled before we leave it; interrupts stay off
         * until the kernel has switched away, and no other thread can run in
         * between, so nothing can reuse it early. */
        ps2_intr_disable();
        DeleteSema(ext->run_sema);
        stack_free(ext->stack, ext->stack_size);
        ExitDeleteThread();
        for (;;)
        {
        }
    }
    TerminateThread(ext->ee_id);
    DeleteThread(ext->ee_id);
    DeleteSema(ext->run_sema);
    stack_free(ext->stack, ext->stack_size);
    ext->magic = 0;
}

void osSetThreadPri(OSThread *t, OSPri pri)
{
    if (t == NULL)
    {
        t = ps2_ultra_self();
    }
    t->priority = pri;
    ChangeThreadPriority(PS2_THREAD_EXT(t)->ee_id, PS2_ULTRA_TO_EE_PRI(pri));
}

OSPri osGetThreadPri(OSThread *t)
{
    if (t == NULL)
    {
        t = ps2_ultra_self();
    }
    return t->priority;
}

OSId osGetThreadId(OSThread *t)
{
    if (t == NULL)
    {
        t = ps2_ultra_self();
    }
    return t->id;
}

void osYieldThread(void)
{
    OSThread *self = ps2_ultra_self();

    RotateThreadReadyQueue(PS2_ULTRA_TO_EE_PRI(self->priority));
}

/* Debug overlay helpers. */
s32 ps2_ultra_coroutine_stacks_in_use(void)
{
    return sCoroutineInUse;
}

s32 ps2_ultra_coroutine_stacks_peak(void)
{
    return sCoroutinePeak;
}
