/* Enumerate hardware without opening it, then exercise the production synthetic
 * capture path. No microphone capture, playback, game or desktop input. */
#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "launcher_session.h"
static int test_background;
#include "launcher_voice_win.h"
int main(void)
{
    int16_t pcm[LS_VOICE_FRAME];
    int ptt = 0, i, peak = 0;
    lv_enum_devices();
    assert(lv_in_count >= 1 && lv_out_count >= 1);
    assert(!strcmp(lv_in_names[0], "Default") && !strcmp(lv_out_names[0], "Default"));
    printf("PASS: WinMM enumeration: %d input and %d output devices (plus defaults)\n",
           lv_in_count - 1, lv_out_count - 1);
    test_background = 1;
    lv_tone_until = 1400;
    assert(lv_capture_frame(0, 0, 0, 0, 1000, pcm, &ptt) && ptt);
    for (i = 0; i < LS_VOICE_FRAME; ++i) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    assert(peak >= 7900 && peak <= 8000);
    assert(!lv_capture_frame(0, 0, 0, 0, 1010, pcm, &ptt));
    assert(lv_capture_frame(0, 0, 0, 0, 1040, pcm, &ptt));
    assert(!lv_capture_frame(0, 0, 0, 0, 1400, pcm, &ptt));
    lv_close();
    assert(!lv_in_open && !lv_out_open);
    puts("PASS: production synthetic source, 40 ms pacing, expiry; no audio device opened");
    return 0;
}
