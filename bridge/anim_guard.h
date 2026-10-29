/* Guarded lookup of a gob's animation state block.
 *
 * The game's lookup (0x44c640) reads table[id] where table is the model resource's byte array at +0x68.
 * A model without animations (exits, triggers, some props) has a NULL table, so asking such a gob for any
 * state reads address <id> and kills the game (Overhaul build: "Jinkies! There was an error"). The game's own
 * callers only ask models that have the animation; the bridge walks every id of every synced gob.
 * Kept free of game addresses so a test can drive it with fakes. */
#ifndef ANIM_GUARD_H
#define ANIM_GUARD_H

typedef BYTE *(*anim_model_fn)(void *gob);
typedef DWORD *(*anim_lookup_fn)(void *gob, int id);

#define ANIM_MODEL_TABLE    0x68    /* model resource: byte per anim id, index into the controller's states or 0xff */

/* model: the game's accessor for a gob's model resource, or NULL when it could not be located (then only
 * the controller check applies). Returns NULL for anything the game could not answer safely. */
static DWORD *anim_state_checked(void *gob, int id, anim_model_fn model, anim_lookup_fn lookup)
{
    BYTE *ctl, *res;

    if (!gob || id < 0 || id >= ANIM_ID_MAX)
        return NULL;
    ctl = *(BYTE **)((BYTE *)gob + GOB_ANIM);
    if (!ctl)
        return NULL;
    if (model) {
        res = model(gob);
        if (!res || !*(BYTE **)(res + ANIM_MODEL_TABLE))
            return NULL;
    }
    return lookup(gob, id);
}
#endif
