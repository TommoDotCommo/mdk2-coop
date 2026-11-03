/* T0 for launcher_look.h: button labels in the game's menu font always fit their button (or fall back to the
 * GDI face), the hero of each level, the steel's mirrored tiling. A synthetic font stands in for font.lua
 * (the game's file is never part of the repository). The COOP_MUTANT_LOOK_NO_FIT build must fail. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../launcher_look.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* every secondary button of the launcher with its size (launcher.c, launcher_join.c): it must keep the game font */
static const struct { const char *s; float w, h; } buttons[] = {
    { "Mute me", 112, 28 }, { "Unmute me", 112, 28 }, { "Deafen", 116, 28 }, { "Undeafen", 116, 28 },
    { "Kick", 44, 25 }, { "Mute", 50, 25 }, { "Muted", 50, 25 }, { "Leave lobby", 392, 64 }, { "Send", 66, 40 },
    { "Copy invite", 170, 32 }, { "Open to my networks", 296, 32 }, { "Invite only", 296, 32 },
    { "Save address", 140, 28 }, { "Waiting for Alice", 476, 64 }, { "x", 34, 34 }, { "Check now", 144, 34 },
    { "Send diagnostics", 220, 34 }, { "Try again", 120, 28 }, { "Update", 100, 28 }, { "Not now", 140, 34 },
    { "Rename", 110, 38 }, { "Unpin", 110, 38 }, { "Forget", 110, 38 }, { "Cancel", 105, 38 },
    { "What's new", 170, 36 }, { "Leave lobby", 233, 52 },
};

int main(void)
{
    float adv[256];
    int i, n;
    unsigned seed = 12345;

    /* about the menu font's widths: capitals and digits half a cell, a space less, wide letters more */
    for (i = 0; i < 256; i++)
        adv[i] = 0.45f;
    adv[' '] = 0.30f;
    adv['M'] = adv['W'] = 0.72f;
    adv['I'] = adv['1'] = 0.25f;

    for (i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
        float gh = ll_label_height(adv, buttons[i].s, buttons[i].w, buttons[i].h);
        CHECK(gh >= LL_MIN_GLYPH, "button \"%s\" (%gx%g) loses the game font (%g)", buttons[i].s, buttons[i].w, buttons[i].h, gh);
        CHECK(gh <= buttons[i].h, "button \"%s\": glyphs %g taller than the button %g", buttons[i].s, gh, buttons[i].h);
        if (gh > 0)
            CHECK(ll_title_width(adv, buttons[i].s, gh) <= buttons[i].w - 2 * LL_PAD, "button \"%s\" runs over its edge",
                  buttons[i].s);
    }
    /* any label, any size: in the menu font only when it fits, never smaller than the minimum, else the GDI face */
    for (n = 0; n < 20000; n++) {
        char s[64];
        int len, k;
        float w, h, gh;
        seed = seed * 1103515245u + 12345u;
        len = 1 + (seed >> 16) % 40;
        for (k = 0; k < len; k++) {
            seed = seed * 1103515245u + 12345u;
            s[k] = (char)(' ' + (seed >> 16) % 95);
        }
        s[len] = 0;
        seed = seed * 1103515245u + 12345u;
        w = 20.0f + (float)((seed >> 16) % 500);
        seed = seed * 1103515245u + 12345u;
        h = 16.0f + (float)((seed >> 16) % 60);
        gh = ll_label_height(adv, s, w, h);
        if (gh > 0) {
            CHECK(ll_title_width(adv, s, gh) <= w - 2 * LL_PAD, "\"%s\" at %g in %gx%g runs over", s, gh, w, h);
            CHECK(gh >= LL_MIN_GLYPH && gh <= LL_MAX_GLYPH && gh <= h * 0.6f + 0.002f, "\"%s\": height %g out of range", s, gh);
        }
        if (fails > 5)
            break;
    }
    /* the short orange buttons: their labels fit too (the tall ones stay at 34, their callers cut the words) */
    {
        static const struct { const char *s; float w, h; } prim[] = {
            { "Install update", 160, 34 }, { "Release page", 160, 34 }, { "Join", 140, 34 }, { "Update", 150, 36 },
            { "Save", 105, 38 }, { "Join", 100, 38 },
        };
        for (i = 0; i < (int)(sizeof(prim) / sizeof(prim[0])); i++) {
            float gh = ll_primary_height(adv, prim[i].s, prim[i].w, prim[i].h);
            CHECK(gh >= LL_MIN_GLYPH && ll_title_width(adv, prim[i].s, gh) <= prim[i].w - 2 * LL_PAD && gh <= prim[i].h - 14,
                  "primary \"%s\" (%gx%g) does not fit at %g", prim[i].s, prim[i].w, prim[i].h, gh);
        }
        CHECK(ll_primary_height(adv, "Open lobby", 476, 64) == 34 && ll_primary_height(adv, "Start game", 233, 52) == 34,
              "the tall primary buttons changed size");
    }
    CHECK(ll_label_height(adv, "A very long label for a tiny button", 60, 28) == 0, "a label that cannot fit keeps the menu font");
    CHECK(ll_label_height(adv, "OK", 60, 20) == 0, "a button too low for the minimum keeps the menu font");

    /* the hero each level starts with (launcher.c level_heroes: Kurt, Max, Doc, ...; 6 and 12 are Doc and Hyde) */
    {
        static const int want[13] = { 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3 };
        for (i = 0; i <= 12; i++)
            CHECK(ll_hero_of_level(i) == want[i], "level %d: hero %d, want %d", i, ll_hero_of_level(i), want[i]);
        CHECK(ll_hero_of_level(13) == LL_HERO_NONE && ll_hero_of_level(-1) == LL_HERO_NONE, "levels out of range have a hero");
        CHECK(!strcmp(ll_portraits[LL_HERO_KURT].name, "Kurt") && !strcmp(ll_portraits[LL_HERO_MAX].name, "Max") &&
              !strcmp(ll_portraits[LL_HERO_DOC].name, "Doc"), "portrait names out of order");
        for (i = 1; i < LL_HEROES; i++)
            CHECK(ll_portraits[i].file && ll_portraits[i].x >= 0 && ll_portraits[i].y >= 0 && ll_portraits[i].size > 16 &&
                  ll_portraits[i].x + ll_portraits[i].size <= 256 && ll_portraits[i].y + ll_portraits[i].size <= 256,
                  "portrait %d outside its 256x256 panel", i);
    }
    /* mirrored tiling: inside 0..n-1, continuous at the joins */
    for (i = -100; i < 100; i++) {
        int m = ll_mirror(i, 7), m2 = ll_mirror(i + 1, 7);
        CHECK(m >= 0 && m < 7, "mirror(%d) = %d", i, m);
        CHECK(abs(m2 - m) <= 1, "mirror jumps at %d: %d to %d", i, m, m2);
    }
    CHECK(ll_mirror(5, 1) == 0, "mirror over one texel");

    if (fails) {
        printf("launcher look: %d failures\n", fails);
        return 1;
    }
    printf("launcher look: labels fit or fall back, heroes, tiling: OK\n");
    return 0;
}
