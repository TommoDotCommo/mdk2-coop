/* Bounded, read-only evidence from the faulting thread. Never dereference game
 * pointers here: the exception may itself be a dangling pointer or bad stack. */
#ifndef CRASH_CONTEXT_H
#define CRASH_CONTEXT_H

static int crash_read(DWORD address, void *out, SIZE_T bytes)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)address,
                             out, bytes, &got) && got == bytes;
}

static void crash_log_context(const CONTEXT *c)
{
    DWORD words[16];
    int i;
    if (!c) return;
    logmsg("crash: registers eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx eip=%08lx eflags=%08lx",
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);
    /* Read words individually: a stack at a page boundary still yields the
     * readable prefix. Raw words are evidence, not an unwound call stack. */
    for (i = 0; i < 16; i++) {
        if (!crash_read(c->Esp + (DWORD)i * 4, &words[i], sizeof(words[i]))) {
            logmsg("crash: stack +%02x unreadable", i * 4);
            break;
        }
        logmsg("crash: stack +%02x %08lx", i * 4, (unsigned long)words[i]);
    }
    /* At this instruction dispatch has subtracted 0x18 and pushed EBP/ESI.
     * Its cdecl arguments are (gob, model, index), not (gob, index).
     * EDX = gob, ECX = state base, EBP = index, ESI = index * 40.
     * Decode only this exact site in the two recognised builds. */
    if ((!strcmp(build_name(), "Overhaul Games build") && c->Eip == 0x4672ed) ||
        (!strcmp(build_name(), "Steam build 3902") && c->Eip == 0x46731d)) {
        DWORD ctl = 0, base = 0, model = 0, index = 0, caller = 0;
        short count = -1;
        int controller_ok = crash_read(c->Edx + GOB_ANIM, &ctl, sizeof(ctl)) && ctl &&
            crash_read(ctl + 0x24, &base, sizeof(base)) &&
            crash_read(ctl + 0x3c, &count, sizeof(count));
        int args_ok = crash_read(c->Esp + 0x20, &caller, sizeof(caller)) &&
            crash_read(c->Esp + 0x28, &model, sizeof(model)) &&
            crash_read(c->Esp + 0x2c, &index, sizeof(index));
        logmsg("crash: animation gob=%08lx ctl=%08lx states=%08lx count=%d controller_read=%d model=%08lx index=%ld caller=%08lx args_read=%d effective=%08lx",
               c->Edx, (unsigned long)ctl, (unsigned long)base, count, controller_ok,
               (unsigned long)model, (long)index, (unsigned long)caller, args_ok,
               c->Ecx + c->Esi + 12);
    }
}
#endif
