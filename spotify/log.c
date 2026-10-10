#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>

#define LOG_PATH "spotify.log"

void log_init(void)
{
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

    pspDebugScreenPrintf("%s", buf);

    /* Open/append/close per call: the FAT size is only committed on close,
     * so a crash or hang still leaves everything logged so far readable. */
    SceUID f = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (f >= 0) {
        sceIoWrite(f, buf, strlen(buf));
        sceIoClose(f);
    }
    return n;
}
