#ifndef PAD_H
#define PAD_H

/* XInput's button bits */
#define PAD_UP      0x0001
#define PAD_DOWN    0x0002
#define PAD_LEFT    0x0004
#define PAD_RIGHT   0x0008
#define PAD_START   0x0010
#define PAD_BACK    0x0020      /* View on an Xbox pad, Back on older ones */
#define PAD_LS      0x0040      /* left stick click */
#define PAD_RS      0x0080      /* right stick click */
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

/* The shared co-op layer on View (standard K4): tap = ping; hold = the player list, and while View is
 * held the D-pad, LB, RB and Y are co-op buttons the game never sees:
 *   D-pad up     emote wheel        (stays open while View is held; right stick picks, letting go of View plays)
 *   D-pad down   chat               (opens the line and a keyboard: Steam's, or ours)
 *   D-pad left / right   watch the previous / next teammate
 *   LB           character wheel    (like the emote wheel)
 *   RB           quick chat wheel   (Ready, Wait for me, ...; like the emote wheel)
 *   Y            quick guide        (while held)
 * One table of these lines is Lua's (coop.padlayer, the Controls page's Gamepad tab and the quick guide). */
#define PL_HOLD_MS  250         /* View held this long (or used with another button) is a hold, not a tap */
#define PL_SWALLOW  (PAD_BACK | PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_LB | PAD_RB | PAD_Y)
enum { PL_NONE, PL_EMOTE, PL_CHAR, PL_QUICK };

struct padlayer {
    int down, used, latch;      /* View held; another layer button used during this hold; a wheel latched open */
    double since;               /* when View went down, ms */
    unsigned prev;              /* last frame's buttons */
};

struct padlayer_out {
    unsigned swallow;           /* pad buttons the game must not see this frame */
    unsigned gk;                /* co-op keys held this frame: 1 << GK_* (bridge.h) */
    int chat;                   /* open the chat line (this frame only) */
    int guide;                  /* the quick guide is held */
};

/* the layer's lines, for every place that lists them (Controls page, quick guide, launcher guide): the buttons,
 * the co-op action they stand for (Lua's coop.actions id, or "quick" / "guide") and what they do */
struct pad_line { const char *buttons, *id, *desc; };
extern const struct pad_line pad_layer_lines[];
extern const int pad_layer_count;

void padlayer_step(struct padlayer *s, unsigned buttons, double now, struct padlayer_out *o);
/* one frame of the pad: the layer first, then what is left of the buttons mapped for the game */
void pad_frame(struct padlayer *s, const struct pad_raw *p, unsigned prev_buttons, float dt, float look, double now,
               struct pad_out *o, struct padlayer_out *lo);
int pad_active(const struct pad_raw *p, unsigned prev_buttons);  /* a button went down or a stick moved */

/* K7, which hints to show: the pad's buttons from the first pad input (a press or a stick moved), the keys again
 * from the first key or mouse button of its own (key_pressed); was: what they showed until now */
int pad_glyphs(int was, const struct pad_raw *p, unsigned prev_buttons, int key_pressed);
const char *pad_glyph_text(int pad, const char *keys, const char *buttons);   /* buttons when pad and set */

void pad_bind(int action, int code);    /* code: input.c's numbering; negative restores the default */
void pad_map(const struct pad_raw *p, unsigned prev_buttons, float dt, float look, struct pad_out *o);
int pad_read(int index, struct pad_raw *out);
void pad_fake(const struct pad_raw *raw);

#endif
