// Head-less native runner for the Omega Amiga core.
//
// Builds the SAME omega/*.c sources the RP2350 firmware uses (with PICO_BUILD
// undefined, so the desktop code paths are taken), links them against a plain
// malloc'd framebuffer and the 16 MB low16Meg array, boots a Kickstart ROM
// plus an ADF, runs the emulator for a while and dumps framebuffer PPMs.
//
// Usage: omega-native [disk.adf] [iterations] [dump_every] [insert_at]
//   disk.adf     ADF image for DF0:              (default: none)
//   iterations   number of 200x(dma+cpu) batches (default: 40000)
//   dump_every   write frameNNNN.ppm every N batches (default: 4000)
//   insert_at    batch index at which to "insert" DF0: (default: 3000;
//                Kickstart must finish drive identification first)
//
// Environment:
//   OMEGA_ROM=<file>   load this Kickstart ROM (256 KB mirrored, or 512 KB)
//                      instead of the built-in Kickstart 1.3.
//   OMEGA_DISASM=1     turn on the Musashi disassembler (to UART/stdout) -
//                      useful for seeing where a ROM's early init diverges.

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
int disass       = 0;     // Musashi disassembler (toggled by OMEGA_DISASM)

// Load a Kickstart ROM file into low16Meg at 0xF80000.
// 512 KB -> straight copy;  256 KB -> mirrored into 0xF80000 and 0xFC0000.
// Returns 0 on success.
static int load_rom_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 1) { printf("  ROM: cannot open %s\n", path); return 1; }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz == 0x40000) {                       // 256 KB
        (void)!read(fd, &low16Meg[0xF80000], 0x40000);
        memcpy(&low16Meg[0xFC0000], &low16Meg[0xF80000], 0x40000);
    } else if (sz == 0x80000) {                // 512 KB
        (void)!read(fd, &low16Meg[0xF80000], 0x80000);
    } else {
        printf("  ROM: %s is %lld bytes (expected 262144 or 524288)\n",
               path, (long long)sz);
        close(fd);
        return 1;
    }
    close(fd);
    printf("  ROM: %s loaded (%lld KB)\n", path, (long long)(sz >> 10));
    return 0;
}

int main(int argc, char **argv) {
    const char  *adfPath   = (argc > 1) ? argv[1] : NULL;
    long         iterations = (argc > 2) ? strtol(argv[2], NULL, 0) : 40000;
    long         dumpEvery  = (argc > 3) ? strtol(argv[3], NULL, 0) : 4000;
    long         insertAt   = (argc > 4) ? strtol(argv[4], NULL, 0) : 3000;
    int          haveDisk   = 0;

    const char *romPath = getenv("OMEGA_ROM");
    if (getenv("OMEGA_DISASM")) disass = 1;

    printf("Omega native runner\n");
    printf("  iterations=%ld  dump_every=%ld  disasm=%d\n",
           iterations, dumpEvery, disass);

    // ── Kickstart ROM into low16Meg at 0xF80000 ─────────────────────────
    if (romPath) {
        if (load_rom_file(romPath)) return 1;
    } else {
        memcpy(&low16Meg[0xF80000], kick13, 524288);   // built-in KS 1.3
        printf("  ROM: built-in Kickstart 1.3\n");
    }
    printf("  ROM: reset vector %02x %02x %02x %02x, entry %08x\n",
           low16Meg[0xF80000], low16Meg[0xF80001],
           low16Meg[0xF80002], low16Meg[0xF80003],
           (low16Meg[0xF80004] << 24) | (low16Meg[0xF80005] << 16) |
           (low16Meg[0xF80006] << 8)  |  low16Meg[0xF80007]);
    if (low16Meg[0xF80000] != 0x11) {
        printf("  ROM: bad signature (expected 0x11)\n");
        return 1;
    }

    // ── DF0: floppy (optional) ──────────────────────────────────────────
    // Always call floppyInit(0) so the drive is properly identified (idMode=-1
    // triggers the 32-pulse ID sequence that Kickstart needs to see before it
    // will attempt disk I/O).  Without this the ROM skips to the ROM disk.
    if (adfPath && *adfPath) {
        int fd = open(adfPath, O_RDONLY);
        if (fd < 1) {
            printf("  DF0: cannot open %s — drive present, no disk\n", adfPath);
            floppyInit(0);
        } else {
            ADF2MFM(fd, floppyInit(0));
            close(fd);
            haveDisk = 1;
            printf("  DF0: %s encoded to MFM (insert at batch %ld)\n",
                   adfPath, insertAt);
        }
    } else {
        // No path or empty path = drive present, no disk
        floppyInit(0);
        printf("  DF0: drive present, no disk\n");
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
