#ifndef PAD_H
#define PAD_H

/* XInput's button bits */
#define PAD_UP      0x0001
#define PAD_DOWN    0x0002
#define PAD_LEFT    0x0004
#define PAD_RIGHT   0x0008
#define PAD_START   0x0010
#define PAD_BACK    0x0020
#define PAD_LB      0x0100
#define PAD_RB      0x0200
#define PAD_A       0x1000
#define PAD_B       0x2000
#define PAD_X       0x4000
#define PAD_Y       0x8000

#define PAD_DEADZONE 0.25f
#define PAD_TRIGGER  40         /* of 255 */

struct pad_raw {
    unsigned buttons;
    int lt, rt;
    int lx, ly, rx, ry;         /* -32768..32767, up and right positive */
};

struct pad_out {
    unsigned char keys[33];     /* held codes, input.c's numbering (0-255 keys, 256+ mouse buttons) */
    int mouse[3];               /* x, y, wheel movement this frame */
};

/* what the pad's buttons press: the keys of these actions (set from the Controls page) */
enum { PA_FWD, PA_BACK, PA_LEFT, PA_RIGHT, PA_JUMP, PA_FIRE, PA_SNIPER, PA_USE, PA_REVIVE, PA_PING, PA_COUNT };

void pad_bind(int action, int code);    /* code: input.c's numbering; negative restores the default */
void pad_map(const struct pad_raw *p, unsigned prev_buttons, float dt, float look, struct pad_out *o);
int pad_read(int index, struct pad_raw *out);
void pad_fake(const struct pad_raw *raw);

#endif
