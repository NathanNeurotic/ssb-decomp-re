/*
 * Logging: printf to the EE/IOP console (visible in PCSX2's log window and on
 * ps2link hosts), a small ring of lines that the boot screen, the debug
 * overlay and the crash screen render on the TV, and a larger text history
 * that ps2_log_save() writes to <boot dir>SSB64.LOG for hardware testing.
 */
#include <ps2/platform.h>

#include <fcntl.h>
#include <kernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_LINES     24
#define LOG_LINE_LEN  80
#define LOG_TEXT_SIZE (48 * 1024)

static char sLines[LOG_LINES][LOG_LINE_LEN];
static int sHead;  /* next slot to write */
static int sCount;
static char sText[LOG_TEXT_SIZE];
static int sTextLen;
static int sConsole = 1;
static int sSaveEnabled;

extern void ps2_gs_show_panic(const char *msg);

void ps2_log_init(void)
{
    sHead = 0;
    sCount = 0;
    sTextLen = 0;
}

static void push_line(const char *s)
{
    int intr = DI();
    int len = (int)strlen(s);

    strncpy(sLines[sHead], s, LOG_LINE_LEN - 1);
    sLines[sHead][LOG_LINE_LEN - 1] = '\0';
    sHead = (sHead + 1) % LOG_LINES;
    if (sCount < LOG_LINES)
    {
        sCount++;
    }

    /* text history: when full, drop the older half */
    if (len > LOG_TEXT_SIZE / 4)
    {
        len = LOG_TEXT_SIZE / 4;
    }
    if (sTextLen + len + 1 > LOG_TEXT_SIZE)
    {
        int keep = sTextLen / 2;

        memmove(sText, sText + sTextLen - keep, (size_t)keep);
        sTextLen = keep;
    }
    memcpy(sText + sTextLen, s, (size_t)len);
    sTextLen += len;
    sText[sTextLen++] = '\n';

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

    if (sConsole)
    {
        printf("[ssb64] %s\n", buf);
    }
    push_line(buf);
}

/* The console may go through an IOP RPC; the crash handler turns it off. */
void ps2_log_console(int enable)
{
    sConsole = enable;
}

void ps2_log_enable_save(int enable)
{
    sSaveEnabled = enable;
}

void ps2_log_save(void)
{
    char path[288];
    int fd;

    if (!sSaveEnabled)
    {
        return;
    }
    ps2_storage_path(path, sizeof(path), "SSB64.LOG");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        return;
    }
    write(fd, sText, (size_t)sTextLen);
    close(fd);
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
    ps2_log_save();

    for (;;)
    {
        SleepThread();
    }
}
