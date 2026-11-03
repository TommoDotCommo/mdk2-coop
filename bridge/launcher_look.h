/*
 * The launcher's game look, the parts that need no window (T0: tests/launcher_look_test.c).
 *
 * Labels of buttons are drawn in the game's menu font (font.dds, widths from font.lua, both read from
 * the player's own mdk2.tar at run time) at a height that fits the button; a label that does not fit
 * even at the smallest height is drawn in the launcher's GDI face instead. Player rows of the lobby
 * show the hero a player starts the chosen level as, cut from the game's own comic panels.
 * Nothing from the game is in this file: only the names of the textures and where to cut them.
 */
#ifndef LAUNCHER_LOOK_H
#define LAUNCHER_LOOK_H

#include <ctype.h>

#define LL_MIN_GLYPH 14.0f      /* smaller than this the menu font smears: the GDI face reads better */
#define LL_MAX_GLYPH 26.0f      /* as the tabs; the primary button is 34 */
#define LL_PAD       5.0f       /* room left and right of a label inside its button */

/* the width of s in the menu font at height h (cell 32x32, advances in cells; the font has capitals only) */
static float ll_title_width(const float adv[256], const char *s, float h)
{
    float w = 0;
    for (; *s; s++)
        w += adv[(unsigned char)toupper((unsigned char)*s)] * h + h * 0.06f;
    return w;
}

/* gh, or smaller until s fits w less the padding; 0: not even LL_MIN_GLYPH fits */
static float ll_fit(const float adv[256], const char *s, float w, float gh)
{
#ifndef COOP_MUTANT_LOOK_NO_FIT
    while (gh >= LL_MIN_GLYPH && ll_title_width(adv, s, gh) > w - 2 * LL_PAD)
        gh -= 1.0f;
#endif
    return gh >= LL_MIN_GLYPH ? gh : 0.0f;
}

/* the glyph height for label s in a secondary button w x h, or 0: it does not fit in the menu font, use the GDI face */
static float ll_label_height(const float adv[256], const char *s, float w, float h)
{
    /* the capitals fill about two thirds of the 32-pixel cell: a cell of 0.78 h gives capitals half the button's height;
     * whole pixels, so 25 * 0.78 does not become 19.4999 */
    float gh = (float)(int)(h * 0.78f + 0.001f);
    return ll_fit(adv, s, w, gh > LL_MAX_GLYPH ? LL_MAX_GLYPH : gh);
}

/* the orange primary button: 34 as always in the tall ones (the label there is cut to fit by its caller), smaller in
 * the short ones (Install update, Join, Save) so the label stays inside */
static float ll_primary_height(const float adv[256], const char *s, float w, float h)
{
    float gh = (float)(int)(h * 0.78f + 0.001f);
    return h >= 50 ? 34 : ll_fit(adv, s, w, gh);
}

/* the heroes; the level scripts create one each (Kurt, Max, Doc; Doc's levels 6 and 12 add Hyde) */
enum { LL_HERO_NONE, LL_HERO_KURT, LL_HERO_MAX, LL_HERO_DOC, LL_HEROES };

/* the hero every player starts level l as (the game's own choice for the level; the character wheel comes later) */
static int ll_hero_of_level(int l)
{
    static const unsigned char hero[13] = { 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3 };
    return l >= 1 && l <= 12 ? hero[l] : LL_HERO_NONE;
}

/* the heroes' faces in the game's comic panels (DDS from mdk2.tar, top-down pixels): file, square x, y, size */
static const struct ll_portrait { const char *file; int x, y, size; const char *name; } ll_portraits[LL_HEROES] = {
    { NULL, 0, 0, 0, "" },
    { "l0apanel004.dds", 134, 112, 84, "Kurt" },
    { "l0xpanel003.dds", 64, 0, 88, "Max" },
    { "l0ypanel004.dds", 40, 4, 88, "Doc" },
};

/* the steel of the buttons: the diamond plate of a panel texture, a band x, y, w, h (mirrored to tile) */
#define LL_PLATE_FILE "jd_panel00.dds"
static const int ll_plate[4] = { 120, 352, 300, 28 };

/* mirrored repeat of v over n texels: 0..n-1..0, so tiles meet without a seam */
static int ll_mirror(int v, int n)
{
    int p = 2 * n;
    if (n <= 1)
        return 0;
    v %= p;
    if (v < 0)
        v += p;
    return v < n ? v : p - 1 - v;
}

#endif
