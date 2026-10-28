#ifndef MDK2_LAUNCHER_VOICE_WIN_H
#define MDK2_LAUNCHER_VOICE_WIN_H

#include <math.h>
#include <mmsystem.h>

#define LV_MAX_DEVS 16
#define LV_NBUF     4

static char lv_in_storage[LV_MAX_DEVS][40];
static const char *lv_in_names[LV_MAX_DEVS];
static int lv_in_count;

static char lv_out_storage[LV_MAX_DEVS][40];
static const char *lv_out_names[LV_MAX_DEVS];
static int lv_out_count;

static HWAVEIN lv_hin;
static HWAVEOUT lv_hout;
static WAVEHDR lv_in_hdr[LV_NBUF], lv_out_hdr[LV_NBUF];
static int16_t lv_in_buf[LV_NBUF][LS_VOICE_FRAME];
static int16_t lv_out_buf[LV_NBUF][LS_VOICE_FRAME];
static int lv_in_open, lv_in_failed, lv_out_open, lv_out_failed;
static int lv_in_opened_dev = -2, lv_out_opened_dev = -2;
static DWORD lv_tone_until, lv_next_tone_ms;

static void lv_wave_format(WAVEFORMATEX *f)
{
    memset(f, 0, sizeof(*f));
    f->wFormatTag = WAVE_FORMAT_PCM;
    f->nChannels = 1;
    f->nSamplesPerSec = LS_VOICE_RATE;
    f->wBitsPerSample = 16;
    f->nBlockAlign = 2;
    f->nAvgBytesPerSec = LS_VOICE_RATE * 2;
}

static void lv_enum_devices(void)
{
    UINT i, nin = 0, nout = 0;

    snprintf(lv_in_storage[0], sizeof(lv_in_storage[0]), "Default");
    lv_in_names[0] = lv_in_storage[0];
    lv_in_count = 1;

    snprintf(lv_out_storage[0], sizeof(lv_out_storage[0]), "Default");
    lv_out_names[0] = lv_out_storage[0];
    lv_out_count = 1;

    if (!test_background) {
        nin = waveInGetNumDevs();
        for (i = 0; i < nin && lv_in_count < LV_MAX_DEVS; i++) {
            WAVEINCAPSA caps;
            if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && caps.szPname[0]) {
                snprintf(lv_in_storage[lv_in_count], sizeof(lv_in_storage[0]), "%.38s", caps.szPname);
                lv_in_names[lv_in_count] = lv_in_storage[lv_in_count];
                lv_in_count++;
            }
        }
        nout = waveOutGetNumDevs();
        for (i = 0; i < nout && lv_out_count < LV_MAX_DEVS; i++) {
            WAVEOUTCAPSA caps;
            if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && caps.szPname[0]) {
                snprintf(lv_out_storage[lv_out_count], sizeof(lv_out_storage[0]), "%.38s", caps.szPname);
                lv_out_names[lv_out_count] = lv_out_storage[lv_out_count];
                lv_out_count++;
            }
        }
    }
}

static void lv_close(void)
{
    int i;
    if (lv_in_open) {
        waveInStop(lv_hin);
        waveInReset(lv_hin);
        for (i = 0; i < LV_NBUF; i++)
            waveInUnprepareHeader(lv_hin, &lv_in_hdr[i], sizeof(WAVEHDR));
        waveInClose(lv_hin);
        lv_hin = NULL;
        lv_in_open = 0;
    }
    if (lv_out_open) {
        waveOutReset(lv_hout);
        for (i = 0; i < LV_NBUF; i++)
            waveOutUnprepareHeader(lv_hout, &lv_out_hdr[i], sizeof(WAVEHDR));
        waveOutClose(lv_hout);
        lv_hout = NULL;
        lv_out_open = 0;
    }
    lv_in_failed = lv_out_failed = 0;
    lv_in_opened_dev = lv_out_opened_dev = -2;
    lv_tone_until = lv_next_tone_ms = 0;
    memset(lv_in_buf, 0, sizeof(lv_in_buf));
    memset(lv_out_buf, 0, sizeof(lv_out_buf));
}

static int lv_open_mic(int dev_choice)
{
    WAVEFORMATEX f;
    UINT dev = (dev_choice > 0 && (UINT)(dev_choice - 1) < waveInGetNumDevs()) ? (UINT)(dev_choice - 1) : WAVE_MAPPER;
    int i;

    if (test_background) return 0;
    if (lv_in_open && lv_in_opened_dev == dev_choice) return 1;
    if (lv_in_open) {
        waveInStop(lv_hin);
        waveInReset(lv_hin);
        for (i = 0; i < LV_NBUF; i++)
            waveInUnprepareHeader(lv_hin, &lv_in_hdr[i], sizeof(WAVEHDR));
        waveInClose(lv_hin);
        lv_hin = NULL;
        lv_in_open = 0;
    }
    lv_wave_format(&f);
    if (waveInOpen(&lv_hin, dev, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        lv_in_failed = 1;
        return 0;
    }
    for (i = 0; i < LV_NBUF; i++) {
        memset(&lv_in_hdr[i], 0, sizeof(lv_in_hdr[i]));
        lv_in_hdr[i].lpData = (LPSTR)lv_in_buf[i];
        lv_in_hdr[i].dwBufferLength = sizeof(lv_in_buf[i]);
        waveInPrepareHeader(lv_hin, &lv_in_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(lv_hin, &lv_in_hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(lv_hin);
    lv_in_open = 1;
    lv_in_opened_dev = dev_choice;
    return 1;
}

static int lv_open_speakers(int dev_choice)
{
    WAVEFORMATEX f;
    UINT dev = (dev_choice > 0 && (UINT)(dev_choice - 1) < waveOutGetNumDevs()) ? (UINT)(dev_choice - 1) : WAVE_MAPPER;
    int i;

    if (test_background) return 0;
    if (lv_out_open && lv_out_opened_dev == dev_choice) return 1;
    if (lv_out_open) {
        waveOutReset(lv_hout);
        for (i = 0; i < LV_NBUF; i++)
            waveOutUnprepareHeader(lv_hout, &lv_out_hdr[i], sizeof(WAVEHDR));
        waveOutClose(lv_hout);
        lv_hout = NULL;
        lv_out_open = 0;
    }
    lv_wave_format(&f);
    if (waveOutOpen(&lv_hout, dev, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        lv_out_failed = 1;
        return 0;
    }
    for (i = 0; i < LV_NBUF; i++) {
        memset(&lv_out_hdr[i], 0, sizeof(lv_out_hdr[i]));
        lv_out_hdr[i].lpData = (LPSTR)lv_out_buf[i];
        lv_out_hdr[i].dwBufferLength = sizeof(lv_out_buf[i]);
        waveOutPrepareHeader(lv_hout, &lv_out_hdr[i], sizeof(WAVEHDR));
        lv_out_hdr[i].dwFlags |= WHDR_DONE;
    }
    lv_out_open = 1;
    lv_out_opened_dev = dev_choice;
    return 1;
}

static void lv_play_frame(int dev_choice, const int16_t *pcm)
{
    int i;
    if (test_background || lv_out_failed) return;
    if (!lv_open_speakers(dev_choice)) return;
    for (i = 0; i < LV_NBUF; i++) {
        WAVEHDR *h = &lv_out_hdr[i];
        if (!(h->dwFlags & WHDR_DONE)) continue;
        memcpy(lv_out_buf[i], pcm, sizeof(lv_out_buf[i]));
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(lv_hout, h, sizeof(WAVEHDR));
        break;
    }
}

/* Capture next 40 ms PCM frame if available; returns 1 when out_pcm + out_ptt are filled. */
static int lv_capture_frame(int dev_choice, int mode_vad, int ptt_key_down, int keep_draining,
                            DWORD now, int16_t *out_pcm, int *out_ptt)
{
    int i;
    if (test_background) {
        int active = (lv_tone_until && (int32_t)(lv_tone_until - now) > 0) || ptt_key_down;
        if (!active && !keep_draining) {
            lv_next_tone_ms = 0;
            return 0;
        }
        if (lv_next_tone_ms && (int32_t)(now - lv_next_tone_ms) < 0)
            return 0;
        lv_next_tone_ms = now + 40;
        if (active) {
            for (i = 0; i < LS_VOICE_FRAME; i++)
                out_pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979323846 * 440.0 * i / LS_VOICE_RATE));
            *out_ptt = 1;
        } else {
            memset(out_pcm, 0, LS_VOICE_FRAME * sizeof(int16_t));
            *out_ptt = 0;
        }
        return 1;
    }
    if (lv_in_failed) return 0;
    if ((mode_vad || ptt_key_down) && !lv_in_open)
        lv_open_mic(dev_choice);
    if (!lv_in_open) return 0;
    for (i = 0; i < LV_NBUF; i++) {
        WAVEHDR *h = &lv_in_hdr[i];
        if (!(h->dwFlags & WHDR_DONE)) continue;
        if (h->dwBytesRecorded == sizeof(lv_in_buf[i])) {
            memcpy(out_pcm, lv_in_buf[i], sizeof(lv_in_buf[i]));
            *out_ptt = ptt_key_down;
            waveInAddBuffer(lv_hin, h, sizeof(WAVEHDR));
            return 1;
        }
        waveInAddBuffer(lv_hin, h, sizeof(WAVEHDR));
    }
    return 0;
}

#endif
