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

/* The AI sight check at 0x402a40 (ebx = actor, edi = target + 0x18) faults when the target the getter
 * at 0x42ad50 returns has no game object at gob+0x84. Overhaul build only: the handles are read from
 * its fixed data (0x548468 AI target, 0x548488 player; gob table 0x523e10, { gob, u16 generation }). */
static void crash_log_gob(const char *what, DWORD gob)
{
    char name[33] = {0};
    DWORD info = 0;
    WORD flags = 0;
    int name_ok = crash_read(gob + GOB_NAME, name, 32), info_ok = crash_read(gob + GOB_INFO, &info, 4),
        flags_ok = crash_read(gob + GOB_FLAGS, &flags, 2);
    logmsg("crash: %s gob=%08lx name='%s' info=%08lx flags=%04x read=%d%d%d", what, gob, name_ok ? name : "?",
           info_ok ? info : 0, flags_ok ? flags : 0, name_ok, info_ok, flags_ok);
}

static void crash_log_sight(const CONTEXT *c)
{
    static const DWORD handle_at[2] = { 0x548468, 0x548488 };
    static const char *handle_name[2] = { "ai target", "player" };
    int i;
    crash_log_gob("sight actor", c->Ebx);
    crash_log_gob("sight target", c->Edi - GOB_POSITION);
    for (i = 0; i < 2; i++) {
        DWORD h = 0, entry[2] = {0, 0};
        int h_ok = crash_read(handle_at[i], &h, 4), e_ok = h_ok && (h & 0xffff) < GOB_SLOTS &&
            crash_read(0x523e10 + (h & 0xffff) * 8, entry, 8);
        logmsg("crash: %s handle=%08lx slot=%lu table gob=%08lx generation=%04lx (handle generation %04lx) read=%d%d",
               handle_name[i], h, h & 0xffff, entry[0], entry[1] & 0xffff, h >> 16, h_ok, e_ok);
    }
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
    if (!strcmp(build_name(), "Overhaul Games build") && c->Eip == 0x402a95) crash_log_sight(c);
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
