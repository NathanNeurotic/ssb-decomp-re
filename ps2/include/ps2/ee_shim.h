/*
 * ps2/include/ps2/ee_shim.h
 *
 * The N64 headers (PR/ultratypes.h) define u32/s32 as `long`, while ps2sdk's
 * tamtypes.h defines them as `int`; both are 32-bit on the EE ABI but the C
 * compiler rejects the conflicting typedefs. Files that implement libultra
 * semantics therefore include the game headers and use these hand-written,
 * ABI-identical declarations of the few EE kernel services they need instead
 * of <kernel.h>.
 */
#ifndef PS2_EE_SHIM_H
#define PS2_EE_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_ee_thread
{
    int status;
    void *func;
    void *stack;
    int stack_size;
    void *gp_reg;
    int initial_priority;
    int current_priority;
    unsigned int attr;
    unsigned int option;
} ps2_ee_thread_t;

typedef struct ps2_ee_sema
{
    int count;
    int max_count;
    int init_count;
    int wait_threads;
    unsigned int attr;
    unsigned int option;
} ps2_ee_sema_t;

extern int CreateThread(ps2_ee_thread_t *thread);
extern int DeleteThread(int thread_id);
extern int StartThread(int thread_id, void *args);
extern void ExitThread(void);
extern void ExitDeleteThread(void);
extern int TerminateThread(int thread_id);
extern int ChangeThreadPriority(int thread_id, int priority);
extern int RotateThreadReadyQueue(int priority);
extern int GetThreadId(void);
extern int SleepThread(void);
extern int WakeupThread(int thread_id);
extern int iWakeupThread(int thread_id);
extern int CancelWakeupThread(int thread_id);
extern int SuspendThread(int thread_id);
extern int ResumeThread(int thread_id);

extern int CreateSema(ps2_ee_sema_t *sema);
extern int DeleteSema(int sema_id);
extern int SignalSema(int sema_id);
extern int iSignalSema(int sema_id);
extern int WaitSema(int sema_id);
extern int PollSema(int sema_id);
extern int iPollSema(int sema_id);

extern int DIntr(void);
extern int EIntr(void);

extern void FlushCache(int operation);
extern void SyncDCache(void *start, void *end);
extern void InvalidDCache(void *start, void *end);

extern void *_gp;

/* Disable interrupts; returns nonzero if they were enabled before. */
static inline int ps2_intr_disable(void)
{
    return DIntr();
}

static inline void ps2_intr_restore(int was_enabled)
{
    if (was_enabled)
    {
        EIntr();
    }
}

/* Interrupt handlers never call the thread-context libultra functions; they
 * use the explicit *_isr variants declared in ps2/ultra.h. */

#ifdef __cplusplus
}
#endif

#endif /* PS2_EE_SHIM_H */
