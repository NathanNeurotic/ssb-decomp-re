#ifndef TEST_PS2_KERNEL_H
#define TEST_PS2_KERNEL_H
typedef struct ee_sema_t
{
    int init_count;
    int max_count;
} ee_sema_t;
int CreateSema(ee_sema_t *sema);
int WaitSema(int id);
int SignalSema(int id);
#endif
