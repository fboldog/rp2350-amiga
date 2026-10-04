// HDMI audio path: see hdmi_audio.h.
//
// Paula produces 48 kHz samples per second of *emulated* time; HDMI plays
// 48000 per second of real time. When the emulation is slower (RemGame runs
// at about half speed) the gap is filled by WSOLA time-stretching, which
// keeps the pitch: the output is built from 768-sample windows (16 ms) with
// 50 % overlap and Hann cross-fades, one every 384 output samples. Each new
// window starts where the input has advanced to (input rate x 384 samples
// since the last one) moved by up to +-192 samples to the position whose
// waveform best matches the natural continuation of the previous window, so
// the cross-fade joins similar waveforms. At full speed the rate is 1 and
// the best match is the exact continuation: the audio passes through
// unchanged. The match is a cross-correlation on a 4x decimated mono signal.
//
// Everything runs from RAM on core 1: hdmi_audio_service() in the thread
// loop, hdmi_audio_take() in the line interrupt.
#include "hdmi_audio.h"

#include <stdbool.h>
#include "pico.h"

#define IN_RING 2048u          // input from Paula (frames, power of two)
#define OUT_RING 512u          // stretched output for the line interrupt
#define HOP 384u               // output samples per window
#define WIN (2u * HOP)         // window length
#define SEARCH 192u            // +- search range
#define DECIM 4u               // correlation decimation
// Input kept behind the newest sample so a window plus the search fits.
#define LAG (WIN + SEARCH + 64u)

static int16_t in_ring[IN_RING][2];
static volatile uint32_t in_head, in_tail;    // free running
static int16_t out_ring[OUT_RING][2];
static volatile uint32_t out_head, out_tail;

// Hann window, rising half (0..32767) over HOP samples; the falling half is
// 32768 minus it, so the two overlapping windows always sum to one.
static int16_t __not_in_flash("hdmi_audio") fade_in[HOP];
static bool fade_ready;

// Stretcher state (core 1 thread).
static uint32_t prev_seg;      // input start of the previous window
static uint32_t in_pos;        // nominal input start of the next window, 16.16 above in_tail
static uint32_t in_pos_int;    // integer input position of the next window
static uint32_t rate;          // input samples per output sample, 16.16
static uint32_t last_head;     // in_head at the previous window
static bool primed;            // a previous window exists
static int32_t gain;           // 0..32768, for fading in/out on stalls
// Near real time (rate within 2 %, left again beyond 3 %) the input is
// copied straight through: no search, no cross-fade, bit-exact. Readable
// over SWD.
volatile bool hdmi_audio_bypass;

static inline int16_t in_s(uint32_t i, int ch) {
    return in_ring[i & (IN_RING - 1u)][ch];
}

void __not_in_flash_func(hdmi_audio_put)(const int16_t *samples, int frames) {
    uint32_t head = in_head;
    const uint32_t tail = __atomic_load_n(&in_tail, __ATOMIC_ACQUIRE);
    for (int i = 0; i < frames && head - tail < IN_RING; ++i, ++head) {
        int16_t *slot = in_ring[head & (IN_RING - 1u)];
        slot[0] = samples ? samples[2 * i] : 0;
        slot[1] = samples ? samples[2 * i + 1] : 0;
    }
    __atomic_store_n(&in_head, head, __ATOMIC_RELEASE);
}

static void __not_in_flash_func(fade_init)(void) {
    // sin^2 ramp without libm: integrate a parabola-free approximation via
    // the recurrence for cos(2*pi*k/(2*HOP)).
    const float w = 3.14159265f / (float)HOP;
    float c = 1.0f, s = 0.0f;
    const float cw = 1.0f - w * w / 2.0f + w * w * w * w / 24.0f;
    const float sw = w - w * w * w / 6.0f + w * w * w * w * w / 120.0f;
    for (uint32_t k = 0; k < HOP; ++k) {
        // rising Hann half: (1 - cos(pi*k/HOP)) / 2
        fade_in[k] = (int16_t)((1.0f - c) * 0.5f * 32767.0f);
        const float c2 = c * cw - s * sw;
        s = s * cw + c * sw;
        c = c2;
    }
    fade_ready = true;
}

// Cross-correlation of the decimated mono signal: input at `a` against
// input at `b`, over the overlap (HOP samples).
static int32_t __not_in_flash_func(correlate)(uint32_t a, uint32_t b) {
    int32_t sum = 0;
    for (uint32_t i = 0; i < HOP; i += DECIM) {
        const int32_t x = (in_s(a + i, 0) + in_s(a + i, 1)) >> 2;
        const int32_t y = (in_s(b + i, 0) + in_s(b + i, 1)) >> 2;
        sum += (x * y) >> 6;
    }
    return sum;
}

// Emits one window step (HOP output samples) if input and room allow.
static bool __not_in_flash_func(stretch_step)(void) {
    const uint32_t head = __atomic_load_n(&in_head, __ATOMIC_ACQUIRE);
    if (OUT_RING - (out_head - __atomic_load_n(&out_tail, __ATOMIC_ACQUIRE)) <
        HOP)
        return false;                       // output full
    if (!primed) {
        if (head - in_tail < LAG + HOP)
            return false;                   // collect some input first
        prev_seg = head - LAG;
        in_pos_int = prev_seg + HOP;
        in_pos = 0;
        last_head = head;
        rate = 1u << 16;
        primed = true;
    }
    // Input rate: arrivals since the last window per output sample,
    // smoothed; nudged so the read position stays ~LAG behind the input.
    const uint32_t arrived = head - last_head;
    last_head = head;
    uint32_t want = (arrived << 16) / HOP;
    const int32_t lag = (int32_t)(head - in_pos_int);
    want = (uint32_t)((int32_t)want + (lag - (int32_t)LAG) * 16);
    if ((int32_t)want < 0)
        want = 0;
    if (want > (3u << 16) / 2u)
        want = (3u << 16) / 2u;
    rate += ((int32_t)want - (int32_t)rate) / 8;

    // Paula stalled (rate under a quarter): fade out instead of looping.
    const bool stalled = rate < (1u << 16) / 4u;

    const int32_t dev = (int32_t)rate - (1 << 16);
    const int32_t adev = dev < 0 ? -dev : dev;
    if (hdmi_audio_bypass ? adev > (3 << 16) / 100 : adev < (2 << 16) / 100)
        hdmi_audio_bypass = !hdmi_audio_bypass;
    const uint32_t next = prev_seg + HOP;   // natural continuation
    if (hdmi_audio_bypass && !stalled &&
        (int32_t)(head - next) >= (int32_t)HOP) {
        // Straight copy: the continuation is the previous window's second
        // half, so the stream stays seamless across mode changes.
        uint32_t oh = out_head;
        for (uint32_t i = 0; i < HOP; ++i, ++oh) {
            int16_t *o = out_ring[oh & (OUT_RING - 1u)];
            if (gain >= 32768) {
                o[0] = in_s(next + i, 0);
                o[1] = in_s(next + i, 1);
            } else {
                o[0] = (int16_t)((in_s(next + i, 0) * gain) >> 15);
                o[1] = (int16_t)((in_s(next + i, 1) * gain) >> 15);
                gain += 64;
            }
        }
        __atomic_store_n(&out_head, oh, __ATOMIC_RELEASE);
        prev_seg = next;
        in_pos_int = next + HOP;
        in_pos = 0;
        const uint32_t keep = next;          // nothing older is needed
        if ((int32_t)(keep - in_tail) > 0)
            __atomic_store_n(&in_tail, keep, __ATOMIC_RELEASE);
        return true;
    }

    // Best start within +-SEARCH of the nominal position, matching the
    // natural continuation of the previous window (prev_seg + HOP).
    uint32_t nominal = in_pos_int;
    if (head - nominal < WIN + SEARCH)
        nominal = head - (WIN + SEARCH);      // never read past the input
    const uint32_t natural = prev_seg + HOP;
    uint32_t best = nominal;
    int32_t best_c = INT32_MIN;
    for (int32_t d = -(int32_t)SEARCH; d <= (int32_t)SEARCH; d += 2 * (int32_t)DECIM) {
        const uint32_t cand = nominal + (uint32_t)d;
        if ((int32_t)(cand - __atomic_load_n(&in_tail, __ATOMIC_ACQUIRE)) < 0)
            continue;
        const int32_t c = correlate(natural, cand);
        if (c > best_c) {
            best_c = c;
            best = cand;
        }
    }
    // Overlap-add: falling half of the previous window + rising half of
    // the new one.
    uint32_t oh = out_head;
    for (uint32_t i = 0; i < HOP; ++i, ++oh) {
        const int32_t up = fade_in[i];
        const int32_t down = 32767 - up;
        int16_t *o = out_ring[oh & (OUT_RING - 1u)];
        for (int ch = 0; ch < 2; ++ch) {
            int32_t v = (in_s(natural + i, ch) * down +
                         in_s(best + i, ch) * up) >> 15;
            v = (v * gain) >> 15;
            o[ch] = (int16_t)v;
        }
        gain += stalled ? -64 : 64;          // ~0.5 ms fades
        if (gain < 0)
            gain = 0;
        if (gain > 32768)
            gain = 32768;
    }
    __atomic_store_n(&out_head, oh, __ATOMIC_RELEASE);
    prev_seg = best;
    // Next nominal start: rate x HOP further along the input, but never
    // beyond what the input can supply (a window plus the search range).
    in_pos += rate * HOP;
    in_pos_int += in_pos >> 16;
    in_pos &= 0xffffu;
    if ((int32_t)(head - in_pos_int) < (int32_t)(WIN + SEARCH)) {
        in_pos_int = head - (WIN + SEARCH);
        in_pos = 0;
    }
    // Free input no window can use any more: older than both the previous
    // window and the next search range, and never newer than LAG behind the
    // input (so the tail cannot pass the head).
    const uint32_t floor = in_pos_int - SEARCH;
    uint32_t keep = (int32_t)(prev_seg - floor) < 0 ? prev_seg : floor;
    if ((int32_t)(keep - (head - LAG)) > 0)
        keep = head - LAG;
    if ((int32_t)(keep - in_tail) > 0)
        __atomic_store_n(&in_tail, keep, __ATOMIC_RELEASE);
    return true;
}

void __not_in_flash_func(hdmi_audio_service)(void) {
    if (!fade_ready)
        fade_init();
    while (stretch_step())
        ;
}

void __not_in_flash_func(hdmi_audio_take)(int16_t *lr) {
    const uint32_t tail = out_tail;
    if (tail == __atomic_load_n(&out_head, __ATOMIC_ACQUIRE)) {
        lr[0] = lr[1] = 0;                   // underrun
        return;
    }
    const int16_t *s = out_ring[tail & (OUT_RING - 1u)];
    lr[0] = s[0];
    lr[1] = s[1];
    __atomic_store_n(&out_tail, tail + 1u, __ATOMIC_RELEASE);
}
