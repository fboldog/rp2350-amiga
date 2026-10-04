// RP2350 host layer – Phase 1 implementation.
//
// Video:   renders planar→chunky into a PSRAM framebuffer and, when enabled,
//          submits completed frames to the PicoDVI scanout running on core 1.
// Input:   USB HID via TinyUSB (src/usb_input.c, USB host builds).
// Serial:  printf → UART via pico_stdio_uart (configured in CMakeLists.txt).

#include "Host.h"
#include "HostRing.h"
#if OMEGA_HDMI_AUDIO
#include "hdmi_audio.h"
#define AUDIO_SERVICE() hdmi_audio_service()
#else
#define AUDIO_SERVICE() ((void)0)
#endif
#if OMEGA_ENABLE_USB_HOST
#include "usb_input.h"
#endif
#include "Presentation.h"
#include "psram.h"
#include "../omega/DisplayLayout.h"
#if OMEGA_ENABLE_HDMI
#include "dvi_display.h"
#endif
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/Audio.h"
#include "../omega/CPU.h"
#include "../omega/DMA.h"
#include "../omega/VideoStandard.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <string.h>

Host_t host;

// Framebuffer lives in PSRAM
static uint32_t *fb;        // presented SCREEN_W * SCREEN_H image
static uint32_t *render_fb; // intermediate DMA beam raster

int16_t hostRasterRowMin[HOST_RASTER_H];
int16_t hostRasterRowMax[HOST_RASTER_H];
// Set while render_fb may hold stale pixels outside the written ranges.
static bool raster_has_stale_pixels;

static void hostResetRasterWritten(void) {
    for (int row = 0; row < HOST_RASTER_H; ++row) {
        hostRasterRowMin[row] = HOST_RASTER_W;
        hostRasterRowMax[row] = 0;
    }
}

// The presentation fallback expects every unwritten raster pixel to hold
// the border colour. Restore that after frames that skipped the clear.
static void hostBorderUnwrittenPixels(uint32_t border) {
    for (int row = 0; row < HOST_RASTER_H; ++row) {
        uint32_t *line = &render_fb[row * HOST_RASTER_W];
        int written_min = hostRasterRowMin[row];
        int written_max = hostRasterRowMax[row];
        if (written_max <= written_min)
            written_min = written_max = HOST_RASTER_W;
        for (int x = 0; x < written_min; ++x)
            line[x] = border;
        for (int x = written_max; x < HOST_RASTER_W; ++x)
            line[x] = border;
    }
}

#if OMEGA_ENABLE_HDMI
// Direct indexed rendering: for full-width and plain narrow fetch layouts,
// bitplane pixels go straight into the free SRAM scanout frame as colour
// register numbers (HAM as raw codes), so the ARGB raster in PSRAM is
// neither written nor read. Scanout expands them to exact 12-bit colours
// (dvi_indexed_frame_t in dvi_display.h).
//
// The conversion runs on core 1. Core 0 (the emulator) only enqueues each
// fetched bitplane block, palette updates and frame begin/end into a
// single-producer/single-consumer ring of 32-bit words; core 1 drains it in
// order between its HSTX line interrupts and logs every palette change
// against the pixels drawn after it, so Copper palette changes still hit the
// right pixels. Core 1 owns the scanout frame, its record and its palette
// copy.
#define DIRECT_MAX_ROWS DVI_MAX_ROWS
static bool direct_frame;         // this frame renders straight to the frame
int hostDirectActive;             // mirrors direct_frame for DMA.c

#define SPRITE_X_BIAS 64   // raster x + bias fits the 10-bit field
// Message format: HostRing.h.

// Core 1 waits for a pending frame to be shown while core 0 keeps queueing;
// when the ring is full core 0 stalls until the swap. A bigger ring lets
// emulation run further ahead of the display. 64 KB holds a whole frame's
// messages across the wait, so every emulated frame is shown (idle
// Workbench emulated/shown: PAL 28.2/21.7 with 24 KB, 28.1/21.9 with 32 KB,
// 28.0/28.0 with 64 KB; NTSC 33.6/32.5 with 44 KB, 33.6/33.6 with 64 KB;
// larger rings change nothing). Any size works: positions run over
// [0, 2 * RING_WORDS).
#ifndef RING_WORDS
#define RING_WORDS 16384u // 64 KB
#endif
#define RING_SPAN (2u * RING_WORDS)
// Ring fill at which core 1 stops waiting for the pending frame's swap and
// skips drawing the new frame. The default skips once core 0 can no longer
// queue the largest message (MSG_MAX_WORDS), i.e. just before it would stall;
// RING_WORDS never skips, which locks emulation to the display cadence (PAL
// idle Workbench: 25.0/25.0 emulated/shown instead of ~29.5/20.5, as a frame
// is several times the ring and cannot absorb the wait for the swap).
#ifndef OMEGA_RING_SKIP_WORDS
#define OMEGA_RING_SKIP_WORDS (RING_WORDS - MSG_MAX_WORDS)
#endif
static uint32_t ring[RING_WORDS];
static volatile uint32_t ring_head; // produce position (core 0)
static volatile uint32_t ring_tail; // consume position (core 1)

static inline uint32_t ringAdvance(uint32_t pos, uint32_t words) {
    pos += words;
    return pos >= RING_SPAN ? pos - RING_SPAN : pos;
}

static inline uint32_t ringFill(uint32_t head, uint32_t tail) {
    return head >= tail ? head - tail : head + RING_SPAN - tail;
}

// Ring slot of position pos (< RING_SPAN).
static inline uint32_t ringSlot(uint32_t pos) {
    return pos >= RING_WORDS ? pos - RING_WORDS : pos;
}

// The run being collected on core 0 (host_run, see HostRing.h). It is
// written straight into the ring past ring_head and published by advancing
// ring_head (runFlush()), so core 1 never sees it early; only a run that
// could reach the end of the ring is collected in run_stage and copied in
// (core 1 copies such a message out).
static uint32_t run_stage[MSG_MAX_WORDS];
HostRun host_run = { run_stage, 0, 0, ~0u };

// ── Core 0: producer ──────────────────────────────────────────────────────
static inline void ringPush(const uint32_t *words, uint32_t count) {
    uint32_t head = ring_head;
    // Wait for room: core 1 converts faster than the emulator fetches, so
    // this only waits after a burst.
    while (ringFill(head, __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE)) +
           count > RING_WORDS)
        tight_loop_contents();
    uint32_t slot = ringSlot(head);
    if (slot + count <= RING_WORDS) {
        for (uint32_t i = 0; i < count; ++i)
            ring[slot + i] = words[i];
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            ring[slot] = words[i];
            if (++slot == RING_WORDS)
                slot = 0;
        }
    }
    __atomic_store_n(&ring_head, ringAdvance(head, count), __ATOMIC_RELEASE);
    __sev(); // wake core 1 from __wfe()
}

static void __attribute__((noinline)) runPushStaged(void) {
    ringPush(run_stage, host_run.words);
}

static inline void runFlush(void) {
    if (!host_run.words)
        return;
    if (host_run.msg == run_stage) {
        runPushStaged();
    } else {
        __atomic_store_n(&ring_head, ringAdvance(ring_head, host_run.words),
                         __ATOMIC_RELEASE);
        __sev(); // wake core 1 from __wfe()
    }
    host_run.words = 0;
}

// Starts a run with this header where it will be published.
static void __attribute__((noinline)) runStart(uint32_t header) {
    // Room for the longest run, as ringPush() would wait for it.
    const uint32_t head = ring_head;
    while (ringFill(head, __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE)) +
           MSG_MAX_WORDS > RING_WORDS)
        tight_loop_contents();
    const uint32_t slot = ringSlot(head);
    host_run.msg = slot + MSG_MAX_WORDS <= RING_WORDS ? &ring[slot] : run_stage;
    host_run.msg[0] = header;
    host_run.words = 1;
}

// Appends a block at x of `words` plane words to the run, first sending the
// run if the block does not continue it, and returns where to write the
// plane words. header has x = 0 and count 1.
static inline uint32_t *runAppend(uint32_t header, int x, int width,
                                  uint32_t words) {
    if (host_run.words && (x != host_run.next_x ||
                      (host_run.msg[0] & MSG_RUN_KEY_MASK) != header ||
                      (host_run.msg[0] & MSG_RUN_COUNT_MASK) == MSG_RUN_COUNT_MASK))
        runFlush();
    if (host_run.words)
        host_run.msg[0] += 1u << MSG_RUN_SHIFT;
    else
        runStart(header | MSG_HEADER(0, 0, x, 0));
    uint32_t *planes = &host_run.msg[host_run.words];
    host_run.words += words;
    host_run.next_x = x + width;
    return planes;
}

// internal.palette holds OCS2ARGB() colours: the high nibbles are the
// original 0x0RGB register value.
static inline uint32_t argbToOcs(uint32_t argb) {
    return (argb >> 12 & 0xf00u) | (argb >> 8 & 0xf0u) | (argb >> 4 & 0xfu);
}

// Out of line, like the other rare paths, so the per-block producers stay
// small (no large stack frame or register saves on every block).
static void __attribute__((noinline)) hostDirectSendPalette(void) {
    host_run.palette_sent = internal.paletteGeneration;
    runFlush(); // the blocks before the change use the old palette
    uint32_t msg[17];
    msg[0] = MSG_HEADER(MSG_PALETTE, 0, 0, 0);
    for (int i = 0; i < 16; ++i)
        msg[1 + i] = argbToOcs(internal.palette[2 * i]) |
                     argbToOcs(internal.palette[2 * i + 1]) << 16;
    ringPush(msg, count_of(msg));
}

static inline void hostDirectSyncPalette(void) {
    if (internal.paletteGeneration != host_run.palette_sent)
        hostDirectSendPalette();
}

// Palette generation as of the last row-end message (core 1 fills the rows
// in between with that message's position: nothing changed there).
static uint32_t row_end_generation;

static void hostDirectBegin(void) {
    runFlush();
    const bool full_width = omegaDdfIsFullWidth(chipset.ddfstrt);
    // Narrow layouts that need hostPresentFrame()'s wrapped-fetch
    // reconstruction (pixels moved between lines) keep the ARGB path.
    const bool narrow = !full_width &&
        !omegaDiwCrossesVerticalBank(chipset.diwstrt, chipset.diwstop);
    direct_frame = full_width || narrow;
    hostDirectActive = direct_frame;
    if (!direct_frame)
        return;
    row_end_generation = host_run.palette_sent;   // core 1's palette
    // Rows are beam lines (DMA.c: line - OMEGA_DIRECT_FIRST_LINE), so every
    // layout maps one raster row to one image row.
    const int step = 1, first_row = 0;
    const int rotation =
        narrow ? 0 : omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop);
    int height = dvi_display_direct_height();
    if (height > DIRECT_MAX_ROWS)
        height = DIRECT_MAX_ROWS;
    const uint32_t msg[3] = {
        MSG_HEADER(MSG_BEGIN, 0, 0, 0),
        (uint16_t)first_row | (uint32_t)step << 16 | (uint32_t)narrow << 24,
        (uint32_t)rotation | (uint32_t)height << 16,
    };
    ringPush(msg, count_of(msg));
}

// A colour change goes to core 1 with its beam row and raster column, so it
// takes effect there instead of at the next block (ATK's colour bars: the
// Copper changes COLOR00 every 22 colour clocks, which block boundaries
// rounded to 64 or 96 columns). Earlier changes not sent yet (between
// frames) go as the full palette first.
void hostDirectColour(int reg, uint16_t value) {
    // A change before this row's blocks so far (most Copper changes, made
    // in the horizontal blank) is exact with the palette sync before the
    // next block, as before; only one inside them needs its position.
    // (A message per change cost RemGame ~3 %.)
    const int row = internal.vPos - OMEGA_DIRECT_FIRST_LINE;
    int x = dmaBeamColumn();
    if (row != host.rasterRow || x >= host.rasterX)
        return;
    if (host_run.palette_sent != internal.paletteGeneration - 2u) {
        hostDirectSendPalette();
        return;
    }
    host_run.palette_sent = internal.paletteGeneration;
    runFlush();
    x = x < 0 ? 0 : x > 0x3ff ? 0x3ff : x;
    const uint32_t msg[2] = { MSG_HEADER(MSG_COLOUR, row, x, reg), value };
    ringPush(msg, count_of(msg));
}

// Only rows without blocks need their own colour position (rows with
// blocks take their border from their first run), and only when the
// palette changed since the last message: RemGame sends none, ATK four.
void hostDirectRowEnd(int row, int drawn) {
    if (drawn || internal.paletteGeneration == row_end_generation)
        return;
    row_end_generation = internal.paletteGeneration;
    hostDirectSyncPalette();
    runFlush();
    runStart(MSG_HEADER(MSG_ROW_END, row, 0, 0));   // a one-word message
    runFlush();
}

void hostDirectHires(int row, int x, uint16_t p1, uint16_t p2,
                     uint16_t p3, uint16_t p4) {
    hostDirectSyncPalette();
    uint32_t *planes = runAppend(MSG_HEADER(MSG_HIRES, row, 0, 0), x, 16, 2);
    planes[0] = p1 | (uint32_t)p2 << 16;
    planes[1] = p3 | (uint32_t)p4 << 16;
}

void hostDirectLores(int row, int x, uint16_t p1, uint16_t p2, uint16_t p3,
                     uint16_t p4, uint16_t p5, uint16_t p6, int ham) {
    hostDirectSyncPalette();
    uint32_t *planes =
        runAppend(MSG_HEADER(MSG_LORES, row, 0, ham ? 1 : 0), x, 32, 3);
    planes[0] = p1 | (uint32_t)p2 << 16;
    planes[1] = p3 | (uint32_t)p4 << 16;
    planes[2] = p5 | (uint32_t)p6 << 16;
}

void hostDirectSprite(int row, int x, int colour_base, int attached,
                      int behind, uint16_t a, uint16_t b, uint16_t c,
                      uint16_t d) {
    const int field = x + SPRITE_X_BIAS;
    if (field < 0 || field > 0x3ff || row < 0 || row > 0x1ff)
        return;
    hostDirectSyncPalette();
    runFlush(); // sprites go over the blocks already sent
    const uint32_t msg[3] = {
        MSG_HEADER(MSG_SPRITE, row, field,
                   (uint32_t)colour_base | (uint32_t)(attached != 0) << 5 |
                   (uint32_t)(behind != 0) << 6),
        a | (uint32_t)b << 16,
        c | (uint32_t)d << 16,
    };
    ringPush(msg, attached ? 3u : 2u);
}

static void hostDirectFinish(void) {
    hostDirectSyncPalette();
    runFlush();
    const uint32_t msg[2] = {
        MSG_HEADER(MSG_END, 0, 0, 0),
        argbToOcs(internal.palette[0]),
    };
    ringPush(msg, count_of(msg));
    static bool first_frame_queued;
    if (!first_frame_queued) {
        first_frame_queued = true;
        printf("DVI: first indexed emulator frame queued\n");
    }
}

// ── Core 1: consumer ──────────────────────────────────────────────────────
static uint8_t *c1_image;          // acquired scanout frame, or NULL
static dvi_indexed_frame_t *c1_record; // its segments and palette log
static int c1_open_row;            // last image row with row_segment set
static bool c1_ham;                // the block being placed holds HAM codes
static int c1_first_row;           // image row of raster row 0
static int c1_step;                // raster rows per image row
static int c1_rotation;            // DDF row rotation in raster columns
static bool c1_narrow;             // narrow fetch layout
static int c1_height;              // image rows
static int c1_mark_row;            // last row with a row-end message
static uint16_t c1_mark_pos;       // its palette-log position
// Core 1's loops must not become libc memset/memcpy calls (flash code).
#define C1_FUNC(name) \
    __attribute__((optimize("no-tree-loop-distribute-patterns"))) \
    __not_in_flash_func(name)
static uint16_t c1_palette[32];   // current colours, 0x0RGB
volatile uint32_t hostCaptureHold; // debug: non-zero stops drawing new frames



// Table-driven planar-to-index: c2p_spread[b] holds the 8 pixels of plane
// byte b as bytes 0/1 (leftmost pixel = bit 7 = lowest byte), so OR-ing the
// entries of all planes, shifted by plane number, yields 8 colour indices
// at once. Pixel order matches *Planar2Chunky: bits 7..0 of the low byte of
// each plane word first, then bits 15..8.
static uint32_t c2p_spread[256][2];

static void hostInitPlanarTables(void) {
    for (int b = 0; b < 256; ++b) {
        uint32_t lo = 0, hi = 0;
        for (int k = 0; k < 4; ++k) {
            lo |= (uint32_t)((b >> (7 - k)) & 1) << (8 * k);
            hi |= (uint32_t)((b >> (3 - k)) & 1) << (8 * k);
        }
        c2p_spread[b][0] = lo;
        c2p_spread[b][1] = hi;
    }
}

#define C2P(p, shift, half) c2p_spread[((p) >> (shift)) & 0xffu][half]

static void C1_FUNC(c1Record)(int y, int x0, int x1);

// Overlays a sprite on image row y (core 0 has clipped it to the display
// window). Columns the bitplanes did not draw on this row get colour 0
// first and are recorded as drawn. Sprite pixels carry 0x40: scanout shows
// them as colour registers even on HAM rows, where they must not change the
// held colour.
static void C1_FUNC(c1Sprite)(int row, int x, uint32_t extra, uint32_t ab,
                              uint32_t cd) {
    if (!c1_image || row % c1_step)
        return;
    const int y = c1_first_row + row / c1_step;
    if (y < 0 || y >= c1_height || y != c1_open_row)
        return;
    const dvi_indexed_frame_t *rec = c1_record;
    const uint32_t first = rec->row_segment[y];
    const uint32_t used = rec->segments_used;
    int min = DVI_DIRECT_IMAGE_WIDTH, max = 0;
    for (uint32_t i = first; i < used; ++i) {
        const int x0 = (int)DVI_SEGMENT_X0(rec->segment[i]);
        const int x1 = (int)DVI_SEGMENT_X1(rec->segment[i]);
        if (x0 < min) min = x0;
        if (x1 > max) max = x1;
    }
    uint8_t *line = c1_image + y * DVI_DIRECT_IMAGE_WIDTH;
    if (min > max)
        min = max = 0;  // nothing drawn on this row yet
    const uint32_t base = extra & 0x1fu;
    const bool attached = (extra >> 5) & 1u;
    const bool behind = (extra >> 6) & 1u;
    for (int i = 0; i < 16; ++i) {
        const int bit = 15 - i;
        uint32_t v = ((ab >> bit) & 1u) | ((ab >> (15 + bit)) & 2u);
        if (attached)
            v |= ((cd >> bit) & 1u) << 2 | ((cd >> (16 + bit)) & 1u) << 3;
        if (!v)
            continue;
        const uint8_t colour = (uint8_t)(0x40u | (base + v));
        for (int j = 0; j < 2; ++j) {
            int ix = x + 2 * i + j;
            if (c1_narrow) {
                ix -= HOST_FETCH_LEAD;
            } else {
                if (ix < 0 || ix >= HOST_RASTER_W)
                    continue;
                ix -= c1_rotation;
                if (ix < 0)
                    ix += HOST_RASTER_W;
            }
            if (ix < 0 || ix >= DVI_DIRECT_IMAGE_WIDTH)
                continue;
            if (ix < min || ix >= max) {
                // Outside the drawn columns: extend the row's drawn range
                // with colour 0 up to this pixel, keeping it contiguous.
                int x0, x1;
                if (min == max) { x0 = ix; x1 = ix + 1; }
                else if (ix < min) { x0 = ix; x1 = min; }
                else { x0 = max; x1 = ix + 1; }
                for (int k = x0; k < x1; ++k)
                    line[k] = 0;
                const bool ham = c1_ham;
                c1_ham = false;
                c1Record(y, x0, x1);
                c1_ham = ham;
                if (min == max) { min = x0; max = x1; }
                else if (x0 < min) min = x0;
                else max = x1;
            }
            // Behind the playfield: only over colour 0 (or other sprites).
            if (behind && line[ix] != 0 && !(line[ix] & 0x40u))
                continue;
            line[ix] = colour;
        }
    }
}

// Records that image columns [x0, x1) of row y were drawn with the palette
// as logged so far. Runs drawn back to back with no palette change between
// them merge.
static void C1_FUNC(c1Record)(int y, int x0, int x1) {
    dvi_indexed_frame_t *rec = c1_record;
    const uint32_t used = rec->segments_used;
    // Rows arrive in beam order; a row behind the open one (e.g. after a
    // mid-frame DIWSTRT change) cannot be filed any more.
    if (y < c1_open_row)
        return;
    while (c1_open_row < y)
        rec->row_segment[++c1_open_row] = (uint16_t)used;
    const uint32_t pos = rec->log_used;
    if (used > rec->row_segment[y]) {
        uint32_t *last = &rec->segment[used - 1u];
        if (DVI_SEGMENT_X1(*last) == (uint32_t)x0 &&
            DVI_SEGMENT_POS(*last) == pos &&
            DVI_SEGMENT_HAM(*last) == (uint32_t)c1_ham) {
            *last = DVI_SEGMENT(DVI_SEGMENT_X0(*last), x1, pos, c1_ham);
            return;
        }
    }
    if (used == DVI_MAX_SEGMENTS) {
        rec->segment_overflow = true;
        return;
    }
    rec->segment[used] = DVI_SEGMENT(x0, x1, pos, c1_ham);
    rec->segments_used = (uint16_t)(used + 1u);
}

static inline void C1_FUNC(c1Copy)(uint8_t *line,
                                               const uint8_t *pixels,
                                               int y, int x, int count) {
    for (int i = 0; i < count; ++i)
        line[x + i] = pixels[i];
    c1Record(y, x, x + count);
}

// Places `width` colour numbers of raster (row, x) into the scanout frame.
static void C1_FUNC(c1Place)(int row, int x,
                                         const uint8_t *pixels, int width) {
    if (!c1_image || row % c1_step)
        return;
    const int y = c1_first_row + row / c1_step;
    if (y < 0 || y >= c1_height)
        return;
    uint8_t *line = c1_image + y * DVI_DIRECT_IMAGE_WIDTH;
    if (c1_narrow) {
        int image_x = x - HOST_FETCH_LEAD;
        int first = 0, count = width;
        if (image_x < HOST_VISIBLE_X0) {
            first = HOST_VISIBLE_X0 - image_x;
            image_x = HOST_VISIBLE_X0;
        }
        if (image_x + (count - first) > HOST_VISIBLE_X1)
            count = first + HOST_VISIBLE_X1 - image_x;
        if (count > first)
            c1Copy(line, pixels + first, y, image_x, count - first);
        return;
    }
    int image_x = x - c1_rotation;
    if (image_x < 0)
        image_x += HOST_RASTER_W;
    int first = HOST_RASTER_W - image_x;
    if (first > width)
        first = width;
    c1Copy(line, pixels, y, image_x, first);
    if (first < width) // the block wraps around the rotated row
        c1Copy(line, pixels + first, y, 0, width - first);
}

static void C1_FUNC(c1Hires)(int row, int x, uint32_t p12,
                                         uint32_t p34) {
    const uint16_t p1 = (uint16_t)p12, p2 = (uint16_t)(p12 >> 16);
    const uint16_t p3 = (uint16_t)p34, p4 = (uint16_t)(p34 >> 16);
    uint8_t pixels[16];
    for (int half = 0; half < 2; ++half) {
        const int shift = half * 8;
        for (int part = 0; part < 2; ++part) {
            uint32_t index = C2P(p1, shift, part) |
                             C2P(p2, shift, part) << 1 |
                             C2P(p3, shift, part) << 2 |
                             C2P(p4, shift, part) << 3;
            uint8_t *out = pixels + half * 8 + part * 4;
            out[0] = (uint8_t)index;
            out[1] = (uint8_t)(index >> 8);
            out[2] = (uint8_t)(index >> 16);
            out[3] = (uint8_t)(index >> 24);
        }
    }
    c1_ham = false;
    c1Place(row, x, pixels, 16);
}

// LORES: 16 pixels of up to 6 planes (EHB palette 32..63, or HAM codes),
// each doubled.
static void C1_FUNC(c1Lores)(int row, int x, uint32_t p12,
                                         uint32_t p34, uint32_t p56,
                                         bool ham) {
    const uint16_t p1 = (uint16_t)p12, p2 = (uint16_t)(p12 >> 16);
    const uint16_t p3 = (uint16_t)p34, p4 = (uint16_t)(p34 >> 16);
    const uint16_t p5 = (uint16_t)p56, p6 = (uint16_t)(p56 >> 16);
    uint8_t pixels[32];
    for (int half = 0; half < 2; ++half) {
        const int shift = half * 8;
        for (int part = 0; part < 2; ++part) {
            uint32_t index = C2P(p1, shift, part) |
                             C2P(p2, shift, part) << 1 |
                             C2P(p3, shift, part) << 2 |
                             C2P(p4, shift, part) << 3 |
                             C2P(p5, shift, part) << 4 |
                             C2P(p6, shift, part) << 5;
            uint8_t *out = pixels + half * 16 + part * 8;
            for (int k = 0; k < 4; ++k) {
                const uint8_t colour = (uint8_t)(index >> (8 * k));
                out[2 * k] = colour;
                out[2 * k + 1] = colour;
            }
        }
    }
    c1_ham = ham;
    c1Place(row, x, pixels, 32);
}

static void C1_FUNC(c1Begin)(uint32_t layout, uint32_t size) {
    c1_first_row = (int16_t)(layout & 0xffffu);
    c1_step = (int)((layout >> 16) & 0xffu);
    c1_narrow = (layout >> 24) & 1u;
    c1_rotation = (int)(size & 0xffffu);
    c1_height = (int)(size >> 16);
    // The previous frame may still wait for the next DVI frame boundary
    // (at most one display frame). Wait for the swap while core 0 keeps
    // queueing; if the ring gets close to full first, skip this frame so
    // the emulator never stalls. A finished frame is never discarded.
    while (dvi_display_frame_pending() &&
           ringFill(ring_head, ring_tail) < OMEGA_RING_SKIP_WORDS)
        AUDIO_SERVICE();
    // hostCaptureHold (set over SWD) freezes the displayed frame so it can
    // be dumped consistently; emulation keeps running.
    c1_image = dvi_display_frame_pending() || hostCaptureHold
             ? NULL
             : dvi_display_direct_acquire(&c1_record); // NULL: boot pattern
    if (!c1_image)
        return;
    dvi_indexed_frame_t *rec = c1_record;
    for (int i = 0; i < 32; ++i)
        rec->initial_palette[i] = c1_palette[i];
    rec->segments_used = 0;
    rec->log_used = 0;
    rec->segment_overflow = false;
    c1_open_row = -1;
    c1_mark_row = -1;
    c1_mark_pos = 0;
}

// Image row `row` has ended: a row without drawn runs shows the colours
// logged so far.
// Rows since the previous message saw no palette change: they keep its
// position.
static void C1_FUNC(c1RowEnd)(int row) {
    if (!c1_image || row % c1_step)
        return;
    const int y = c1_first_row + row / c1_step;
    if (y <= c1_mark_row || y >= c1_height)
        return;
    dvi_indexed_frame_t *rec = c1_record;
    while (++c1_mark_row < y)
        rec->row_log[c1_mark_row] = c1_mark_pos;
    c1_mark_pos = rec->log_used;
    rec->row_log[y] = c1_mark_pos;
}

// Unwritten columns need no fill: scanout shows them in the row's COLOR00.
static void C1_FUNC(c1End)(uint16_t border) {
    if (!c1_image)
        return;
    dvi_indexed_frame_t *rec = c1_record;
    while (c1_open_row < DVI_MAX_ROWS)
        rec->row_segment[++c1_open_row] = rec->segments_used;
    while (++c1_mark_row < c1_height)
        rec->row_log[c1_mark_row] = c1_mark_pos;
    dvi_display_direct_publish(border);
    c1_image = NULL;
}

// Takes the new colours and logs the changed ones for the frame being drawn.
static void C1_FUNC(c1Palette)(const uint32_t *pairs) {
    dvi_indexed_frame_t *rec = c1_record;
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t pair = pairs[i];
        for (uint32_t k = 0; k < 2u; ++k) {
            const uint32_t reg = 2u * i + k;
            const uint16_t colour = (uint16_t)(pair >> (16u * k));
            if (colour == c1_palette[reg])
                continue;
            c1_palette[reg] = colour;
            if (!c1_image)
                continue;
            // Before any row: the frame's starting palette. A full log
            // drops the change for this frame; the next frame starts from
            // the complete palette again.
            if (c1_open_row < 0 && c1_mark_row < 0)
                rec->initial_palette[reg] = colour;
            else if (rec->log_used < DVI_MAX_PALETTE_LOG)
                rec->palette_log[rec->log_used++] = reg << 12 | colour;
        }
    }
}


// Image column of raster column x (as c1Place() maps it).
static inline int C1_FUNC(c1ImageX)(int x) {
    if (c1_narrow)
        return x - HOST_FETCH_LEAD;
    int image_x = x - c1_rotation;
    if (image_x < 0)
        image_x += HOST_RASTER_W;
    return image_x;
}

// Colour register `reg` changed at raster column x of row `row`: log it,
// and the open row's runs drawn before the change show it from x on (a run
// that spans x is split there).
static void C1_FUNC(c1Colour)(int row, int x, int reg, uint16_t value) {
    if (value == c1_palette[reg])
        return;
    c1_palette[reg] = value;
    dvi_indexed_frame_t *rec = c1_record;
    if (!c1_image || rec->log_used >= DVI_MAX_PALETTE_LOG)
        return;
    rec->palette_log[rec->log_used++] = (uint32_t)reg << 12 | value;
    const uint32_t pos = rec->log_used;
    if (row % c1_step ||
        c1_first_row + row / c1_step != c1_open_row || c1_open_row < 0)
        return;
    int c = c1ImageX(x);
    if (c < 0)
        c = 0;
    uint32_t used = rec->segments_used;
    for (uint32_t i = rec->row_segment[c1_open_row]; i < used; ++i) {
        const uint32_t seg = rec->segment[i];
        const uint32_t x0 = DVI_SEGMENT_X0(seg), x1 = DVI_SEGMENT_X1(seg);
        const uint32_t ham = DVI_SEGMENT_HAM(seg);
        if (x1 <= (uint32_t)c)
            continue;
        if (x0 >= (uint32_t)c) {
            rec->segment[i] = DVI_SEGMENT(x0, x1, pos, ham);
            continue;
        }
        if (used == DVI_MAX_SEGMENTS)
            continue;               // no room to split: keeps the old colour
        for (uint32_t j = used; j > i + 1u; --j)
            rec->segment[j] = rec->segment[j - 1u];
        rec->segment[i] = DVI_SEGMENT(x0, (uint32_t)c, DVI_SEGMENT_POS(seg), ham);
        rec->segment[i + 1u] = DVI_SEGMENT((uint32_t)c, x1, pos, ham);
        ++used;
        ++i;
    }
    rec->segments_used = (uint16_t)used;
}

void C1_FUNC(hostCore1Loop)(void) {
    uint32_t tail = ring_tail;
    for (;;) {
        uint32_t head;
        while ((head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE)) == tail) {
            AUDIO_SERVICE();   // HDMI audio stretching between messages
            __wfe();
        }
        unsigned since_audio = 0;
        while (tail != head) {
            if (++since_audio == 32u) {   // keep the audio output ring fed
                since_audio = 0;
                AUDIO_SERVICE();
            }
            // Messages are read in place; only one that may wrap around
            // the end of the ring is first copied out (MSG_MAX_WORDS: the
            // longest).
            uint32_t slot = ringSlot(tail);
            const uint32_t *msg = &ring[slot];
            uint32_t wrapped[MSG_MAX_WORDS];
            if (slot + MSG_MAX_WORDS > RING_WORDS) {
                for (uint32_t i = 0; i < MSG_MAX_WORDS; ++i) {
                    wrapped[i] = ring[slot];
                    if (++slot == RING_WORDS)
                        slot = 0;
                }
                msg = wrapped;
            }
            const uint32_t header = msg[0];
            const int type = (int)(header & 0xfu);
            const int row = (int)((header >> 4) & 0x1ffu);
            const int x = (int)((header >> 13) & 0x3ffu);
            uint32_t length;
            switch (type) {
            case MSG_HIRES: {
                const int count = (int)(header >> MSG_RUN_SHIFT & 0xfu) + 1;
                for (int i = 0; i < count; ++i)
                    c1Hires(row, x + 16 * i, msg[1 + 2 * i], msg[2 + 2 * i]);
                length = 1 + 2 * (uint32_t)count;
                break;
            }
            case MSG_LORES: {
                const int count = (int)(header >> MSG_RUN_SHIFT & 0xfu) + 1;
                const bool ham = (header >> 23) & 1u;
                for (int i = 0; i < count; ++i)
                    c1Lores(row, x + 32 * i, msg[1 + 3 * i], msg[2 + 3 * i],
                            msg[3 + 3 * i], ham);
                length = 1 + 3 * (uint32_t)count;
                break;
            }
            case MSG_PALETTE:
                c1Palette(&msg[1]);
                length = 17;
                break;
            case MSG_COLOUR:
                c1Colour(row, x, (int)(header >> 23 & 31u), (uint16_t)msg[1]);
                length = 2;
                break;
            case MSG_BEGIN:
                c1Begin(msg[1], msg[2]);
                length = 3;
                break;
            case MSG_SPRITE:
                c1Sprite(row, x - SPRITE_X_BIAS, header >> 23, msg[1], msg[2]);
                length = ((header >> 28) & 1u) ? 3 : 2;
                break;
            case MSG_END:
                c1End((uint16_t)msg[1]);
                length = 2;
                break;
            case MSG_ROW_END:
                c1RowEnd(row);
                length = 1;
                break;
            default:
                panic("host: bad core-1 ring message %08lx",
                      (unsigned long)header);
            }
            tail = ringAdvance(tail, length);
            __atomic_store_n(&ring_tail, tail, __ATOMIC_RELEASE);
        }
    }
}
#endif

// Direct frames never reach these: DMA.c sends every block, HAM included,
// through hostDirectHires/Lores.
uint32_t *hostRasterPixels(int row, int x) {
    return &render_fb[row * HOST_RASTER_W + x];
}

void hostRasterWritten(int row, int x, int width) {
    if (x < hostRasterRowMin[row]) hostRasterRowMin[row] = (int16_t)x;
    if (x + width > hostRasterRowMax[row])
        hostRasterRowMax[row] = (int16_t)(x + width);
}

// ── Amiga key-code table (same values as the SDL Host.c) ─────────────────
static const uint8_t keyMapping[] = {
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x41, 0x42, 0x0,  0x0,  0x0,  0x44, 0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x45, 0x0,  0x0,  0x0,  0x0,
    0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x2A,
    0x0,  0x0,  0x0,  0x0,  0x38, 0x0B, 0x39, 0x3A,
    0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0,  0x29, 0x52, 0x0C, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x46, 0x5F, 0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x4E,
    0x4F, 0x4D, 0x4C, 0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x1A, 0x2B, 0x1B, 0x0,  0x0,
    0x0,  0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24,
    0x25, 0x17, 0x26, 0x27, 0x28, 0x37, 0x36, 0x18,
    0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11,
    0x32, 0x15, 0x31, 0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x30,
};

static int LEDActive = 1;
static int keyMask   = 0;

void pressKey(uint16_t keyCode) {
    if (keyCode >= sizeof(keyMapping)) return;
    CIAWrite(&CIAA, 0xC, ~(keyMapping[keyCode] << 1));
    keyboardInt();

    if (keyCode == 224) keyMask |= 0x1;  // Ctrl
    if (keyCode == 227) keyMask |= 0x2;  // Left Amiga
    if (keyCode == 231) keyMask |= 0x4;  // Right Amiga
    if (keyMask == 0x7) cpu_pulse_reset();
}

void releaseKey(uint16_t keyCode) {
    if (keyCode >= sizeof(keyMapping)) return;
    CIAWrite(&CIAA, 0xC, ~((keyMapping[keyCode] << 1) | 1));
    keyboardInt();

    if (keyCode == 224) keyMask &= 0x6;
    if (keyCode == 227) keyMask &= 0x5;
    if (keyCode == 231) keyMask &= 0x3;
}

int hostResetRequested;

// Paula's 48 kHz stereo output (omega/Audio.c), sent over HDMI.
void hostAudioOut(const int16_t *samples, int frames) {
#if OMEGA_HDMI_AUDIO
    hdmi_audio_put(samples, frames);
#else
    (void)samples;
    (void)frames;
#endif
}

// Sends one Amiga raw keycode (0x00-0x67) to the keyboard serial port, as
// the Amiga keyboard does: the byte is rotated left with the up/down flag
// in bit 0 and inverted. Ctrl + both Amiga keys resets the 68000.
void hostAmigaKey(uint8_t code, int released) {
    CIAWrite(&CIAA, 0xC, (uint8_t)~((code << 1) | (released ? 1 : 0)));
    keyboardInt();
    static uint8_t resetKeys;  // bit 0 Ctrl, 1 left Amiga, 2 right Amiga
    const uint8_t bit = code == 0x63 ? 1 : code == 0x66 ? 2 : code == 0x67 ? 4 : 0;
    if (released)
        resetKeys &= (uint8_t)~bit;
    else
        resetKeys |= bit;
    if (resetKeys == 7)
        hostResetRequested = 1;  // done between slices (main loop)
}

void toggleLEDs(void) {
    LEDActive = 1 - LEDActive;
}

// ── Display output stub ───────────────────────────────────────────────────
// PicoDVI consumes completed frames; other display backends remain optional.
static void display_push_frame(void) {
#if OMEGA_ENABLE_HDMI
    static bool first_frame_queued;
    if (dvi_display_submit_frame(fb) && !first_frame_queued) {
        first_frame_queued = true;
        printf("DVI: first emulator frame queued\n");
    }
#else
    (void)fb;
#endif
}

// ── Lifecycle ─────────────────────────────────────────────────────────────
void hostInit(void) {
    fb = (uint32_t *)psram_ptr(PSRAM_FRAMEBUF_OFFSET);
    render_fb = (uint32_t *)psram_ptr(PSRAM_VIDEO_RASTER_OFFSET);
    host.pixels   = render_fb;
    host.rasterRow = 0;
    host.rasterX   = 0;
    host.displayIsLores = 0;
    memset(fb, 0, SCREEN_W * SCREEN_H * sizeof(uint32_t));
    memset(render_fb, 0, HOST_RASTER_PIXELS * sizeof(uint32_t));
    hostResetRasterWritten();
#if OMEGA_ENABLE_HDMI
    direct_frame = false;
    hostDirectActive = 0;
    hostInitPlanarTables();
#endif
    printf("Host init: framebuffer at %p (%d×%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

#if OMEGA_REALTIME
// Real-time limit: on average at most one emulated frame per display refresh (60.0 Hz
// for NTSC, 0.1 % above a real Amiga; 50 Hz for PAL), so screens that
// emulate faster than a real Amiga (idle Workbench) do not run games, music
// and timers too fast. Locking to the display rather than a clock keeps
// the frames in phase with its swaps, so none are skipped. Slower emulation
// never waits.
static void hostPaceFrame(void) {
    // Frames emulated against display refreshes: wait only while ahead.
    // Behind (slow emulation) builds credit for occasional fast frames,
    // capped at two frames so a slow stretch is not followed by a sprint.
    static uint32_t frames;
    const uint32_t refreshes = __atomic_load_n(&dvi_refreshes, __ATOMIC_ACQUIRE);
    if ((int32_t)(refreshes - frames) > 2)
        frames = refreshes - 2;
    ++frames;
    while ((int32_t)(frames - __atomic_load_n(&dvi_refreshes,
                                             __ATOMIC_ACQUIRE)) > 0)
        tight_loop_contents();
}
#endif

static void hostDisplayFrame(void);

// Once per VBL: hand the finished frame over, then wait for real time, so
// the frame reaches core 1 without the wait's delay.
void hostDisplay(void) {
    hostDisplayFrame();
#if OMEGA_REALTIME
    hostPaceFrame();
#endif
}

static void hostDisplayFrame(void) {
#if OMEGA_ENABLE_USB_HOST
    usb_input_frame();  // mouse counters, buttons, one queued key
#endif
#if OMEGA_ENABLE_HDMI
    if (direct_frame) {
        hostDirectFinish();
        // The ARGB raster was not written; older contents are stale.
        raster_has_stale_pixels = true;
        hostResetRasterWritten();
        hostDirectBegin();
        return;
    }
    // The ARGB paths below write scanout frames from core 0; let core 1
    // finish converting the previous direct frame first.
    while (__atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) != ring_head)
        tight_loop_contents();
    if (omegaDdfIsFullWidth(chipset.ddfstrt)) {
        const uint32_t border = internal.palette[0];
        const int alternate_rows =
            host.displayIsLores &&
            omegaLoresUsesAlternateRasterRows(chipset.diwstrt,
                                               chipset.diwstop);
        const int source_step = alternate_rows ? 2 : 1;
        const int y_offset = host.displayIsLores && !alternate_rows ? 64 : 0;
        static bool first_frame_queued;
        if (dvi_display_submit_raster(
                render_fb, hostRasterRowMin, hostRasterRowMax,
                border, source_step, y_offset,
                omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop)) &&
            !first_frame_queued) {
            first_frame_queued = true;
            printf("DVI: first direct-raster emulator frame queued\n");
        }
        // No 1 MB raster clear: the next frame reads only what it writes.
        raster_has_stale_pixels = true;
        hostResetRasterWritten();
        hostDirectBegin();
        return;
    }
#endif
    if (raster_has_stale_pixels) {
        hostBorderUnwrittenPixels(internal.palette[0]);
        raster_has_stale_pixels = false;
    }
    hostPresentFrame(fb, render_fb);
    display_push_frame();
    hostResetRasterWritten();
#if OMEGA_ENABLE_HDMI
    hostDirectBegin();
#endif
}
