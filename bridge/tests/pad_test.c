/* Offline check of the gamepad -> keyboard/mouse mapping (make -C bridge test-pad) */
#include <stdio.h>
#include <stdlib.h>
#define PAD_NO_XINPUT
#include "../pad.c"

void logmsg(const char *fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int down(const struct pad_out *o, int code) { return (o->keys[code / 8] >> (code % 8)) & 1; }

int main(void)
{
    struct pad_raw p = { 0 };
    struct pad_out o;

    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(!down(&o, 200) && !down(&o, 256) && o.mouse[0] == 0 && o.mouse[1] == 0);
    p.ly = 32767; p.lx = -32767;                      /* stick up and left: forward + strafe left */
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(down(&o, 200) && down(&o, 203) && !down(&o, 208));
    p.ly = 6000; p.lx = 0;                            /* inside the dead zone */
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(!down(&o, 200));
    memset(&p, 0, sizeof(p));
    p.buttons = PAD_A | PAD_X | PAD_START; p.rt = 255; p.lt = 255;
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(down(&o, 57) && down(&o, 28) && down(&o, 19) && down(&o, 1) && down(&o, 256) && down(&o, 257));
    p.buttons = PAD_BACK;                             /* ping */
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(!down(&o, 47) && !down(&o, 19));        /* View is the co-op layer now, not a key */
    memset(&p, 0, sizeof(p));
    p.rx = 32767; p.ry = 32767;                       /* full right and up, 1 s */
    pad_map(&p, 0, 1.0f, 700, &o);
    CHECK(o.mouse[0] == 700 && o.mouse[1] == -700);
    memset(&p, 0, sizeof(p));
    p.buttons = PAD_RB;                               /* wheel click only on the press */
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(o.mouse[2] == 120);
    pad_map(&p, PAD_RB, 0.016f, 700, &o);
    CHECK(o.mouse[2] == 0);
    /* the action keys follow the bindings (Original: jump on the right mouse button, sniper on Space) */
    pad_bind(PA_JUMP, 257); pad_bind(PA_SNIPER, 57); pad_bind(PA_FWD, 16);
    memset(&p, 0, sizeof(p));
    p.buttons = PAD_A | PAD_UP; p.lt = 255;
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(down(&o, 257) && down(&o, 28) && down(&o, 57) && down(&o, 16) && down(&o, 200) && !down(&o, 17));
    pad_bind(PA_JUMP, -1); pad_bind(PA_SNIPER, -1); pad_bind(PA_FWD, -1);
    pad_map(&p, 0, 0.016f, 700, &o);
    CHECK(down(&o, 57) && down(&o, 17) && down(&o, 257));
    pad_bind(PA_FWD, -1);

    /* the co-op layer on View, from recorded pad states: { buttons, ms since the start } per frame */
    {
        struct padlayer L;
        struct padlayer_out lo;
        unsigned prev = 0;
        int i, pings = 0, lists = 0, moved = 0, emote = 0, quick = 0, chats = 0, guide = 0, watchn = 0, ch = 0;
        /* a tap: View for 100 ms -> one ping, no list */
        static const struct { unsigned b; int t; } tap[] = { { 0, 0 }, { PAD_BACK, 16 }, { PAD_BACK, 50 },
            { PAD_BACK, 116 }, { 0, 133 }, { 0, 150 } };
        /* a hold: View for 600 ms with the D-pad up pressed at 400 ms -> list, then the emote wheel until View goes */
        static const struct { unsigned b; int t; } hold[] = { { PAD_BACK, 0 }, { PAD_BACK, 100 }, { PAD_BACK, 300 },
            { PAD_BACK | PAD_UP, 400 }, { PAD_BACK, 450 }, { PAD_BACK, 600 }, { 0, 616 } };

        memset(&L, 0, sizeof(L));
        memset(&p, 0, sizeof(p));
        for (i = 0; i < (int)(sizeof(tap) / sizeof(tap[0])); i++) {
            p.buttons = tap[i].b;
            pad_frame(&L, &p, prev, 0.016f, 700, tap[i].t, &o, &lo);
            prev = p.buttons;
            pings += !!(lo.gk & (1u << GK_PING));
            lists += !!(lo.gk & (1u << GK_LIST));
            CHECK(!down(&o, 47));                     /* View never presses the ping key for the game */
        }
        CHECK(pings == 1 && lists == 0);
        memset(&L, 0, sizeof(L));
        prev = 0;
        pings = lists = 0;
        for (i = 0; i < (int)(sizeof(hold) / sizeof(hold[0])); i++) {
            p.buttons = hold[i].b;
            pad_frame(&L, &p, prev, 0.016f, 700, hold[i].t, &o, &lo);
            prev = p.buttons;
            pings += !!(lo.gk & (1u << GK_PING));
            if (i == 1) CHECK(!(lo.gk & (1u << GK_LIST)));      /* 100 ms: not a hold yet */
            if (i == 2) CHECK(lo.gk & (1u << GK_LIST));         /* 300 ms: the player list */
            lists += !!(lo.gk & (1u << GK_LIST));
            emote += !!(lo.gk & (1u << GK_EMOTE));
            moved += down(&o, 200) || down(&o, 17);
        }
        CHECK(pings == 0 && lists == 1);
        CHECK(emote == 3);                            /* latched from the press until View is let go */
        CHECK(moved == 0);                            /* the D-pad did not walk while View was held */
        CHECK(!(lo.gk & (1u << GK_EMOTE)));           /* let go: the wheel closes (and plays its choice) */
        /* without View the D-pad walks as before */
        p.buttons = PAD_UP;
        pad_frame(&L, &p, 0, 0.016f, 700, 1000, &o, &lo);
        CHECK(down(&o, 200) && down(&o, 17) && lo.gk == 0);
        /* View + D-pad down: chat (once); + right: watch next; + RB: quick chat wheel; + LB: the character
         * wheel; + Y: the guide, and Y is not the inventory then. LB / RB do not change the item. */
        memset(&L, 0, sizeof(L));
        prev = 0;
        {
            static const unsigned seq[] = { PAD_BACK, PAD_BACK | PAD_DOWN, PAD_BACK | PAD_DOWN, PAD_BACK | PAD_RIGHT,
                PAD_BACK | PAD_Y, PAD_BACK | PAD_RB, PAD_BACK, 0, PAD_BACK, PAD_BACK | PAD_LB, PAD_BACK, 0 };
            for (i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++) {
                p.buttons = seq[i];
                pad_frame(&L, &p, prev, 0.016f, 700, i * 16.0, &o, &lo);
                prev = p.buttons;
                chats += lo.chat;
                watchn += !!(lo.gk & (1u << GK_WATCHN));
                guide += lo.guide;
                quick += !!(lo.gk & (1u << GK_QUICK));
                ch += !!(lo.gk & (1u << GK_CHAR));
                pings += !!(lo.gk & (1u << GK_PING));
                moved += down(&o, 208) || down(&o, 31) || down(&o, 205) || down(&o, 32) || down(&o, 18) || o.mouse[2];
            }
        }
        CHECK(chats == 1 && watchn == 1 && guide == 1 && quick == 2 && ch == 2 && pings == 0 && moved == 0);
        /* the last input: a pad press or a stick moved */
        memset(&p, 0, sizeof(p));
        CHECK(!pad_active(&p, 0));
        p.rx = 30000;
        CHECK(pad_active(&p, 0));
        p.rx = 0; p.buttons = PAD_A;
        CHECK(pad_active(&p, 0) && !pad_active(&p, PAD_A));
    }
    /* K7: the hints follow the last input at once, from recorded inputs: a key, a pad press, the pad held (no
     * new input: no change), a key, the right stick moved, nothing */
    {
        struct pad_raw idle = { 0 }, a = { 0 }, look = { 0 };
        int g = 0;

        a.buttons = PAD_A;
        look.rx = -30000;
        g = pad_glyphs(g, &idle, 0, 1);
        CHECK(g == 0 && !strcmp(pad_glyph_text(g, "Tab", "View"), "Tab"));
        g = pad_glyphs(g, &a, 0, 0);
        CHECK(g == 1 && !strcmp(pad_glyph_text(g, "Tab", "View"), "View"));
        g = pad_glyphs(g, &a, PAD_A, 0);
        CHECK(g == 1);
        g = pad_glyphs(g, NULL, 0, 1);
        CHECK(g == 0 && !strcmp(pad_glyph_text(g, "R", "Hold X"), "R"));
        g = pad_glyphs(g, &look, 0, 0);
        CHECK(g == 1 && !strcmp(pad_glyph_text(g, "R", "Hold X"), "Hold X"));
        g = pad_glyphs(g, &idle, 0, 0);
        CHECK(g == 1);
        CHECK(!strcmp(pad_glyph_text(1, "F2", ""), "F2"));     /* no pad button for it: the key stays */
    }
    printf(fails ? "%d failed\n" : "pad mapping, co-op layer and glyph switch: all passed\n", fails);
    return fails != 0;
}
