// Head-less native runner for the Omega Amiga core.
//
// Builds the SAME omega/*.c sources the RP2350 firmware uses (with PICO_BUILD
// undefined, so the desktop code paths are taken), links them against a plain
// malloc'd framebuffer and the 16 MB low16Meg array, boots the embedded
// Kickstart 1.3 ROM plus an ADF, runs the emulator for a while and dumps
// framebuffer snapshots as PPM files.
//
// Usage: omega-native [disk.adf] [iterations] [dump_every] [insert_at]
//   disk.adf     ADF image for DF0:              (default: none)
//   iterations   number of 200x(dma+cpu) batches (default: 40000)
//   dump_every   write frameNNNN.ppm every N batches (default: 4000)
//   insert_at    batch index at which to "insert" DF0: (default: 3000;
//                Kickstart must finish drive identification first)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "Memory.h"          // src/Memory.h -> #else branch: extern low16Meg
#include "Host.h"
#include "../omega/CPU.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/DMA.h"
#include "../omega/Floppy.h"

#include "../omega/Kick13.h" // const unsigned char kick13[524288]

extern unsigned long native_frame_counter;
extern unsigned long native_nonblack_pixels(void);
extern void          native_dump_ppm(const char *path);

int screenWidth  = 640;   // referenced by some omega translation units
int screenHeight = 200;
int disass       = 0;     // Musashi disassembler off

int main(int argc, char **argv) {
    const char  *adfPath   = (argc > 1) ? argv[1] : NULL;
    long         iterations = (argc > 2) ? strtol(argv[2], NULL, 0) : 40000;
    long         dumpEvery  = (argc > 3) ? strtol(argv[3], NULL, 0) : 4000;
    long         insertAt   = (argc > 4) ? strtol(argv[4], NULL, 0) : 3000;
    int          haveDisk   = 0;

    printf("Omega native runner\n");
    printf("  iterations=%ld  dump_every=%ld\n", iterations, dumpEvery);

    // ── Kickstart ROM into low16Meg at 0xF80000 (512 KB image) ───────────
    memcpy(&low16Meg[0xF80000], kick13, 524288);
    if (low16Meg[0xF80000] == 0x11) {
        printf("  ROM: Kickstart loaded (first byte 0x11 OK)\n");
    } else {
        printf("  ROM: bad signature 0x%02x\n", low16Meg[0xF80000]);
        return 1;
    }

    // ── DF0: floppy (optional) ──────────────────────────────────────────
    if (adfPath) {
        int fd = open(adfPath, O_RDONLY);
        if (fd < 1) {
            printf("  DF0: cannot open %s\n", adfPath);
        } else {
            ADF2MFM(fd, floppyInit(0));
            close(fd);
            haveDisk = 1;
            printf("  DF0: %s encoded to MFM (insert at batch %ld)\n",
                   adfPath, insertAt);
        }
    } else {
        printf("  DF0: empty\n");
    }

    // ── Bring up the emulator ───────────────────────────────────────────
    hostInit();
    cpu_init();
    ChipsetInit();

    printf("Entering emulation loop\n");
    fflush(stdout);

    int dumpIndex = 0;
    for (long it = 0; it < iterations; ++it) {
        for (int i = 0; i < 200; ++i) {
            dma_execute();
            cpu_execute();
        }

        // Kickstart only accepts a disk-change once the drive has finished
        // ID mode (df[0].idMode == 0).  Keep trying from insertAt onward
        // until the drive latches the disk-present bit (pra & 0x04).
        if (haveDisk && it >= insertAt && !(df[0].pra & 0x04) &&
            (it % 200) == 0) {
            floppyInsert(0);
            if (df[0].pra & 0x04) {
                printf("  it=%6ld  disk inserted (idMode=%d)\n",
                       it, df[0].idMode);
                fflush(stdout);
            }
        }

        if (dumpEvery > 0 && (it % dumpEvery) == 0) {
            char name[64];
            snprintf(name, sizeof name, "frame%04d.ppm", dumpIndex++);
            native_dump_ppm(name);
            printf("  it=%6ld  vbl=%lu  nonblack_px=%lu  -> %s\n",
                   it, native_frame_counter, native_nonblack_pixels(), name);
            fflush(stdout);
        }
    }

    native_dump_ppm("frame_final.ppm");
    printf("Done. vbl=%lu  nonblack_px=%lu  -> frame_final.ppm\n",
           native_frame_counter, native_nonblack_pixels());
    return 0;
}
