/* Windows test: exercise the actual DirectInput wrappers without the game or desktop input. */
#include <assert.h>
#include <stdarg.h>
#include "../bridge/input.c"
struct build_addrs ADDR;
char coop_dir[MAX_PATH] = ".";
double now_ms(void) { return 1000; }
void logmsg(const char *fmt, ...) { (void)fmt; }
void hud_frame(HDC dc) { (void)dc; }
void launcher_run(void) {}
int chat_blocks_keys(void) { return 0; }
int pad_read(int index, struct pad_raw *out) { return 0; }
void pad_map(const struct pad_raw *p, unsigned prev, float dt, float look, struct pad_out *out)
{ memset(out, 0, sizeof(*out)); }

static HRESULT WINAPI real_state(void *self, DWORD size, void *out)
{
    memset(out, 0, size);
    ((BYTE *)out)[17] = 0x80;
    return DI_OK;
}

int main(void)
{
    struct route_input in = {0}, captured_input;
    BYTE keyboard[256];
    DIMOUSESTATE2 mouse;
    DIDEVICEOBJECTDATA events[16];
    DWORD count;
    int token;
    struct dev *keyboard_dev = &devs[0], *mouse_dev = &devs[1];
    keyboard_dev->ptr = &token;
    keyboard_dev->kind = DEV_KEYBOARD;
    mouse_dev->ptr = &mouse;
    mouse_dev->kind = DEV_MOUSE;
    test_background = 1;
    synth = 1;
    in.keys[17/8] = 1 << (17%8);
    in.keys[256/8] = 1 << (256%8);
    in.mouse[0] = 4; in.mouse[1] = -7; in.mouse[2] = 120;
    input_replay(&in);
    assert(dev_get_state(&token, sizeof(keyboard), keyboard) == DI_OK);
    assert(keyboard[17] == 0x80 && keyboard[30] == 0);
    assert(dev_get_state(&mouse, sizeof(mouse), &mouse) == DI_OK);
    assert(mouse.lX == 4 && mouse.lY == -7 && mouse.lZ == 120 && mouse.rgbButtons[0] == 0x80);
    input_capture(&captured_input);
    assert(captured_input.mouse[0] == 4 && captured_input.mouse[1] == -7);
    assert(captured_input.keys[17/8] & (1 << (17%8)));
    dev_get_state(&mouse, sizeof(mouse), &mouse);
    assert(mouse.lX == 0 && mouse.lY == 0 && mouse.lZ == 0); /* consume only once */
    input_replay(NULL);
    dev_get_state(&token, sizeof(keyboard), keyboard);
    assert(keyboard[17] == 0);
    in.mouse[0] = 5; in.mouse[1] = in.mouse[2] = 0;
    input_replay(&in);
    count = 16;
    dev_get_data(&mouse, sizeof(events[0]), events, &count, DIGDD_PEEK);
    assert(count == 2 && events[0].dwOfs == 0 && events[0].dwData == 5);
    count = 1;
    dev_get_data(&mouse, sizeof(events[0]), events, &count, 0);
    assert(count == 1 && events[0].dwData == 5); /* full buffers preserve remaining button */
    count = 16;
    dev_get_data(&mouse, sizeof(events[0]), events, &count, 0);
    assert(count == 1 && events[0].dwOfs == 12 && events[0].dwData == 0x80);
    count = 16;
    dev_get_data(&mouse, sizeof(events[0]), events, &count, 0);
    assert(count == 0);
    input_replay(NULL);
    test_background = 0;
    synth = 0;
    get_state_orig = real_state;
    dev_get_state(&token, sizeof(keyboard), keyboard);
    assert(keyboard[17] == 0x80); /* foreground recording passes real input through */
    input_capture(&captured_input);
    assert(captured_input.keys[17/8] & (1 << (17%8)));
    puts("input: keyboard, mouse axes/buttons, buffered/peek, release, foreground recording PASS");
    return 0;
}
