#pragma once

// Shared compile-time video standard.  Keep these values numeric so both the
// shell build and CMake can pass OMEGA_VIDEO_STANDARD directly to the compiler.
#define OMEGA_VIDEO_NTSC 0
#define OMEGA_VIDEO_PAL  1

#ifndef OMEGA_VIDEO_STANDARD
#define OMEGA_VIDEO_STANDARD OMEGA_VIDEO_NTSC
#endif

#if OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL
#define OMEGA_VIDEO_NAME              "PAL"
#define OMEGA_VIDEO_FRAME_LINES       313
#define OMEGA_VIDEO_VPOSR_ID          0x0000
#define OMEGA_VIDEO_RATE_NUMERATOR    50
#define OMEGA_VIDEO_RATE_DENOMINATOR  1
#define OMEGA_VIDEO_VIEWPORT_Y_OFFSET 96
#elif OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_NTSC
#define OMEGA_VIDEO_NAME              "NTSC"
#define OMEGA_VIDEO_FRAME_LINES       263
#define OMEGA_VIDEO_VPOSR_ID          0x1000
#define OMEGA_VIDEO_RATE_NUMERATOR    60000
#define OMEGA_VIDEO_RATE_DENOMINATOR  1001
#define OMEGA_VIDEO_VIEWPORT_Y_OFFSET 40
#else
#error "OMEGA_VIDEO_STANDARD must be OMEGA_VIDEO_NTSC (0) or OMEGA_VIDEO_PAL (1)"
#endif
