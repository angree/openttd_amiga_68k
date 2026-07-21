/* Minimal no-FPU bisect probe.
 * Logs via dos.library Write() only - never via printf - so the log survives
 * even if printf itself traps. Then calls snprintf and logs again.
 * If the log stops after "step1", snprintf executed a Line-F instruction.
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

static BPTR g_log;

static void L(const char *s)
{
    if (g_log) {
        Write(g_log, (APTR)s, (LONG)strlen(s));
        Write(g_log, (APTR)"\n", 1);
    }
}

int main(void)
{
    char buf[64];

    g_log = Open((STRPTR)"Work:nofpu.log", MODE_NEWFILE);
    if (!g_log) return 20;

    L("step0: log opened, no printf yet");

    /* plain integer work - must never need an FPU */
    {
        volatile int a = 1234, b = 7;
        int c = a / b;
        if (c == 176) L("step1: integer arithmetic ok");
        else          L("step1: integer arithmetic WRONG");
    }

    L("step2: about to call snprintf with %d only");

    /* volatile so the compiler cannot constant-fold the call away */
    {
        volatile int n = 4242;
        const char *s = "text";
        snprintf(buf, sizeof(buf), "%d and %s", (int)n, s);
    }

    L("step3: snprintf returned");
    L(buf);

    L("step4: done - no FPU trap anywhere");

    Close(g_log);
    return 0;
}
