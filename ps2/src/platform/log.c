/*
 * Logging: printf to the EE/IOP console (visible in PCSX2's log window and on
 * ps2link hosts) plus a small ring buffer that the boot screen and the debug
 * overlay render on the TV.
 */
#include <ps2/platform.h>

#include <kernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINES     24
#define LOG_LINE_LEN  80

static char sLines[LOG_LINES][LOG_LINE_LEN];
static int sHead;  /* next slot to write */
static int sCount;

extern void ps2_gs_show_panic(const char *msg);

void ps2_log_init(void)
{
    sHead = 0;
    sCount = 0;
}

static void push_line(const char *s)
{
    int intr = DI();

    strncpy(sLines[sHead], s, LOG_LINE_LEN - 1);
    sLines[sHead][LOG_LINE_LEN - 1] = '\0';
    sHead = (sHead + 1) % LOG_LINES;
    if (sCount < LOG_LINES)
    {
        sCount++;
    }
    if (intr)
    {
        EI();
    }
}

void ps2_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[ssb64] %s\n", buf);
    push_line(buf);
}

int ps2_log_line_count(void)
{
    return sCount;
}

const char *ps2_log_line(int i)
{
    int idx;

    if (i < 0 || i >= sCount)
    {
        return "";
    }
    idx = (sHead - sCount + i + LOG_LINES) % LOG_LINES;
    return sLines[idx];
}

void ps2_panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[ssb64] PANIC: %s\n", buf);
    push_line("*** PANIC ***");
    push_line(buf);
    ps2_gs_show_panic(buf);

    for (;;)
    {
        SleepThread();
    }
}
