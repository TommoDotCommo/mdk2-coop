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
    printf(fails ? "%d failed\n" : "pad mapping: all passed\n", fails);
    return fails != 0;
}
