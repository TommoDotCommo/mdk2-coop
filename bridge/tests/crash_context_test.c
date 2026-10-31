#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../bridge.h"
static char output[8192];
static const char *name = "Overhaul Games build";
const char *build_name(void) { return name; }
void logmsg(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(output);
    va_start(ap, fmt);
    vsnprintf(output + n, sizeof(output) - n, fmt, ap);
    va_end(ap);
    strcat(output, "\n");
}
#ifndef CRASH_CONTEXT_HEADER
#define CRASH_CONTEXT_HEADER "../crash_context.h"
#endif
#include CRASH_CONTEXT_HEADER

int main(void)
{
    CONTEXT c = {0};
    DWORD stack[16] = {0}, gob[40] = {0}, ctl[16] = {0};
    char want[80];
    c.Eax = 0x12345678; c.Ebp = 0x70000000; c.Esi = c.Ebp * 40;
    c.Edx = (DWORD)(uintptr_t)gob; c.Ecx = 0x05000000;
    c.Esp = (DWORD)(uintptr_t)stack; c.Eip = 0x4672ed;
    gob[GOB_ANIM / 4] = (DWORD)(uintptr_t)ctl;
    ctl[0x24 / 4] = c.Ecx; *(short *)((BYTE *)ctl + 0x3c) = 72;
    stack[0x20 / 4] = 0x44dd12;
    stack[0x24 / 4] = c.Edx;
    stack[0x28 / 4] = 0x12341234; /* model, deliberately unlike index */
    stack[0x2c / 4] = c.Ebp;
    crash_log_context(&c);
    assert(strstr(output, "crash: registers eax=12345678"));
    assert(strstr(output, "stack +2c 70000000"));
    assert(strstr(output, "count=72 controller_read=1 model=12341234 index=1879048192 caller=0044dd12 args_read=1"));
    snprintf(want, sizeof(want), "effective=%08lx", c.Ecx + c.Esi + 12);
    assert(strstr(output, want));
    output[0] = 0;
    name = "Steam build 3902"; c.Eip = 0x46731d;
    stack[0x2c / 4] = c.Ebp = (DWORD)-1;
    crash_log_context(&c);
    assert(strstr(output, "index=-1"));
    /* Unrecognised sites/builds get generic evidence, never engine decoding. */
    output[0] = 0; c.Eip++;
    crash_log_context(&c);
    assert(strstr(output, "crash: registers"));
    assert(!strstr(output, "crash: animation"));
    output[0] = 0; name = "unknown"; c.Eip = 0x46731d;
    crash_log_context(&c);
    assert(!strstr(output, "crash: animation"));
    /* Corrupt pointers must not produce a second access violation. */
    output[0] = 0; name = "Overhaul Games build"; c.Eip = 0x4672ed;
    c.Esp = 1; c.Edx = 1;
    crash_log_context(&c);
    assert(strstr(output, "stack +00 unreadable"));
    assert(strstr(output, "controller_read=0"));
    assert(strstr(output, "args_read=0"));
    /* The sight check on a target with no game object (mdk2hd.exe+0x2a95 reading 8). */
    {
        DWORD actor[64] = {0}, target[64] = {0};
        output[0] = 0; memset(&c, 0, sizeof(c)); memset(stack, 0, sizeof(stack));
        c.Eip = 0x402a95; c.Ebx = (DWORD)(uintptr_t)actor; c.Edi = (DWORD)(uintptr_t)target + GOB_POSITION;
        c.Esp = (DWORD)(uintptr_t)stack;
        strcpy((char *)actor + GOB_NAME, "l8r4_dog05"); strcpy((char *)target + GOB_NAME, "coop_p1");
        actor[GOB_INFO / 4] = 0x1234;
        crash_log_context(&c);
        assert(strstr(output, "crash: sight actor gob="));
        assert(strstr(output, "name='l8r4_dog05' info=00001234"));
        assert(strstr(output, "crash: sight target gob="));
        assert(strstr(output, "name='coop_p1' info=00000000"));
        assert(strstr(output, "crash: ai target handle="));
        assert(strstr(output, "crash: player handle="));
        output[0] = 0; c.Ebx = 1; c.Edi = 0x20;   /* unreadable: no second fault */
        crash_log_context(&c);
        assert(strstr(output, "name='?' info=00000000"));
        output[0] = 0; c.Eip = 0x402a96; crash_log_context(&c);
        assert(!strstr(output, "crash: sight"));
        output[0] = 0; c.Eip = 0x402a95; name = "Steam build 3902"; crash_log_context(&c);
        assert(!strstr(output, "crash: sight"));
        name = "Overhaul Games build";
    }
    crash_log_context(NULL);
    puts("PASS: crash registers, stack, animation arguments, build/site gates and unreadable pointers");
    return 0;
}
