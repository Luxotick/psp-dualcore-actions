#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

#define LOG_PATH "spotify.log"

static SceInt64 log_start;
static int at_line_start = 1;
static int echo = 1;
/* UI, player and fetch threads all log; keep lines whole. */
static SceUID log_lock = -1;

void log_set_echo(int on)
{
    echo = on;
}

void log_init(void)
{
    log_start = sceKernelGetSystemTimeWide();
    if (log_lock < 0) log_lock = sceKernelCreateSema("log", 0, 1, 1, NULL);
    SceUID f = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f >= 0) sceIoClose(f);
}

int log_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    if (log_lock >= 0) sceKernelWaitSema(log_lock, 1, NULL);

    if (echo) pspDebugScreenPrintf("%s", buf);

    /* The file copy gets a [seconds.millis] stamp at each line start so
     * hangs and slow steps can be timed afterwards. */
    char stamp[24] = "";
    if (at_line_start && buf[0]) {
        unsigned int ms = (unsigned int)((sceKernelGetSystemTimeWide() - log_start) / 1000);
        snprintf(stamp, sizeof stamp, "[%4u.%03u] ", ms / 1000u, ms % 1000u);
    }
    size_t len = strlen(buf);
    if (len) at_line_start = buf[len - 1] == '\n';

    /* Open/append/close per call: the FAT size is only committed on close,
     * so a crash or hang still leaves everything logged so far readable. */
    SceUID f = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (f >= 0) {
        if (stamp[0]) sceIoWrite(f, stamp, strlen(stamp));
        sceIoWrite(f, buf, len);
        sceIoClose(f);
    }
    if (log_lock >= 0) sceKernelSignalSema(log_lock, 1);
    return n;
}
