//
//  Audio.c
//  Paula audio: four DMA channels, mixed to 48 kHz stereo.
//
//  Each channel plays signed 8-bit samples, high byte of each word first,
//  one sample every AUDxPER colour clocks (DMA slots in this emulator), at
//  AUDxVOL (0-64). With DMA on, starting the channel copies AUDxLC/AUDxLEN
//  into its counters and raises the channel's interrupt, so software can
//  queue the next block; words are fetched as needed, and when the length
//  runs out the registers are reloaded and the interrupt raised again. With
//  DMA off, a CPU write to AUDxDAT plays that word, and the interrupt asks
//  for the next one. Attach modes (ADKCON) are not emulated.
//
//  The channels run in emulated time and are advanced once per raster line;
//  output samples fall at 48 kHz of emulated time, so the pitch is exact
//  relative to the emulated machine whatever the emulation speed.
//

#include "Audio.h"
#include "Chipset.h"
#include "VideoStandard.h"

#define SLOTS_PER_LINE 228   // hPos 0..0xE3
#define MIN_DMA_PERIOD 124   // shortest period audio DMA can feed

typedef struct {
    int mode;           // 0 off, 1 DMA, 2 CPU-written AUDxDAT
    uint32_t ptr;       // next word to fetch (word index into chip RAM)
    uint32_t len;       // words left in the current block
    uint16_t word;      // word being played
    int lowNext;        // its low byte is the next sample
    int64_t counter;    // colour clocks left of the current sample (16.16)
    int8_t sample;      // current output sample
    int datPending;     // CPU wrote AUDxDAT since the last word was taken
} AudioChannel;

static AudioChannel ch[4];
static int active;      // channels with mode != 0 (bit per channel)
// Colour clocks per output sample and until the next one (16.16).
static const uint32_t outStep = (uint32_t)(
    ((uint64_t)SLOTS_PER_LINE * OMEGA_VIDEO_FRAME_LINES *
     OMEGA_VIDEO_RATE_NUMERATOR << 16) /
    ((uint64_t)OMEGA_VIDEO_RATE_DENOMINATOR * AUDIO_RATE));
static int32_t outNext;

static inline uint32_t regLc(int c) {
    return c == 0 ? chipset.aud0lc : c == 1 ? chipset.aud1lc :
           c == 2 ? chipset.aud2lc : chipset.aud3lc;
}
static inline uint16_t regLen(int c) {
    return c == 0 ? chipset.aud0len : c == 1 ? chipset.aud1len :
           c == 2 ? chipset.aud2len : chipset.aud3len;
}
static inline uint16_t regPer(int c) {
    return c == 0 ? chipset.aud0per : c == 1 ? chipset.aud1per :
           c == 2 ? chipset.aud2per : chipset.aud3per;
}
static inline uint16_t regVol(int c) {
    return c == 0 ? chipset.aud0vol : c == 1 ? chipset.aud1vol :
           c == 2 ? chipset.aud2vol : chipset.aud3vol;
}
static inline uint16_t regDat(int c) {
    return c == 0 ? chipset.aud0dat : c == 1 ? chipset.aud1dat :
           c == 2 ? chipset.aud2dat : chipset.aud3dat;
}

static void raiseInterrupt(int c) {
    putChipReg16[INTREQ]((uint16_t)(0x8000u | (0x80u << c)));
}

static void reload(int c) {
    ch[c].ptr = regLc(c);
    ch[c].len = regLen(c) ? regLen(c) : 0x10000u;
    raiseInterrupt(c);
}

static int dmaOn(int c) {
    return (chipset.dmaconr & (0x200u | (1u << c))) == (0x200u | (1u << c));
}

static void setMode(int c, int mode) {
    ch[c].mode = mode;
    if (mode)
        active |= 1 << c;
    else
        active &= ~(1 << c);
}

void audioReset(void) {
    for (int c = 0; c < 4; ++c)
        ch[c] = (AudioChannel){0};
    active = 0;
    outNext = 0;
}

void audioDmaconChanged(uint16_t old) {
    for (int c = 0; c < 4; ++c) {
        const int was = (old & (0x200u | (1u << c))) == (0x200u | (1u << c));
        const int now = dmaOn(c);
        if (now && !was) {
            setMode(c, 1);
            ch[c].lowNext = 0;
            ch[c].counter = 0;
            reload(c);
        } else if (!now && was && ch[c].mode == 1) {
            setMode(c, 0);
            ch[c].sample = 0;
        }
    }
}

void audioDatWritten(int c) {
    if (dmaOn(c))
        return;  // DMA owns the channel
    ch[c].datPending = 1;
    if (ch[c].mode == 0) {
        setMode(c, 2);
        ch[c].lowNext = 0;
        ch[c].counter = 0;
    }
}

// Moves channel c to its next sample.
static void nextSample(int c) {
    AudioChannel *a = &ch[c];
    if (a->lowNext) {
        a->sample = (int8_t)(a->word & 0xFF);
        a->lowNext = 0;
        return;
    }
    if (a->mode == 1) {
        // chip RAM words are stored host-endian: swap to the Amiga's order
        const uint16_t w = internal.chipramW[a->ptr & 0xFFFFFu];
        a->word = (uint16_t)(w << 8 | w >> 8);
        a->ptr += 1;
        if (--a->len == 0)
            reload(c);
    } else {
        // CPU playback: a written word plays once; the interrupt asks for
        // the next, and without one the channel goes idle.
        if (!a->datPending) {
            setMode(c, 0);
            a->sample = 0;
            return;
        }
        a->datPending = 0;
        a->word = regDat(c);
        raiseInterrupt(c);
    }
    a->sample = (int8_t)(a->word >> 8);
    a->lowNext = 1;
}

static void advance(int c, int32_t clocks) {
    AudioChannel *a = &ch[c];
    if (a->mode == 0)
        return;
    a->counter -= clocks;
    while (a->counter <= 0) {
        nextSample(c);
        uint32_t per = regPer(c);
        if (per == 0)
            per = 0x10000;  // the period counter wraps: 0 is the longest
        if (a->mode == 1 && per < MIN_DMA_PERIOD)
            per = MIN_DMA_PERIOD;
        a->counter += (int64_t)per << 16;
        if (a->mode == 0)
            break;
    }
}

static int32_t channelOut(int c) {
    if (ch[c].mode == 0)
        return 0;
    uint32_t vol = regVol(c) & 0x7F;
    if (vol > 64)
        vol = 64;
    return ch[c].sample * (int32_t)vol;   // -8192..8128
}

static int16_t clamp16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void audioLine(void) {
    const int32_t lineClocks = SLOTS_PER_LINE << 16;
    if (!active) {
        // All channels off (most of the time): only count the samples.
        int frames = 0;
        while (outNext < lineClocks) {
            ++frames;
            outNext += (int32_t)outStep;
        }
        outNext -= lineClocks;
        hostAudioOut(0, frames);
        return;
    }
    int16_t out[2 * 4];  // a line holds at most 4 output samples
    int frames = 0;
    int32_t pos = 0;
    while (outNext < lineClocks) {
        const int32_t d = outNext - pos;
        for (int c = 0; c < 4; ++c)
            advance(c, d);
        pos = outNext;
        // Two channels per side, each up to +-8192: x2 uses the 16-bit range.
        out[2 * frames] = clamp16((channelOut(0) + channelOut(3)) * 2);
        out[2 * frames + 1] = clamp16((channelOut(1) + channelOut(2)) * 2);
        ++frames;
        outNext += (int32_t)outStep;
    }
    for (int c = 0; c < 4; ++c)
        advance(c, lineClocks - pos);
    outNext -= lineClocks;
    hostAudioOut(out, frames);
}
