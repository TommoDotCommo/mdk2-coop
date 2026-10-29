/* The guarded animation lookup against fake gobs: a model without an animation table must never reach the
 * game's lookup, which would read address <id>. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../bridge.h"
#ifndef ANIM_GUARD_HEADER
#define ANIM_GUARD_HEADER "../anim_guard.h"
#endif
#include ANIM_GUARD_HEADER

static int lookups;
static BYTE *res_of;

static BYTE *model_of(void *gob) { (void)gob; return res_of; }
static DWORD fake_state[10];
static DWORD *lookup(void *gob, int id) { (void)gob; (void)id; lookups++; return fake_state; }

int main(void)
{
    BYTE gob[0x100] = {0}, ctl[0x40] = {0}, res[0x80] = {0}, table[ANIM_ID_MAX] = {0};

    assert(!anim_state_checked(NULL, 0, model_of, lookup));
    assert(!anim_state_checked(gob, 0, model_of, lookup));                  /* no controller */
    *(BYTE **)(gob + GOB_ANIM) = ctl;
    res_of = NULL;
    assert(!anim_state_checked(gob, 0, model_of, lookup));                  /* no model resource */
    res_of = res;
    assert(!anim_state_checked(gob, 3, model_of, lookup));                  /* model without an animation table */
    assert(lookups == 0);
    *(BYTE **)(res + ANIM_MODEL_TABLE) = table;
    assert(anim_state_checked(gob, 3, model_of, lookup) == fake_state);     /* a model with the table is answered */
    assert(!anim_state_checked(gob, -1, model_of, lookup));
    assert(!anim_state_checked(gob, ANIM_ID_MAX, model_of, lookup));
    assert(lookups == 1);
    assert(anim_state_checked(gob, 0, NULL, lookup) == fake_state);         /* accessor unknown: controller check only */
    puts("PASS: guarded animation lookup");
    return 0;
}
