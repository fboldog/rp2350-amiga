#pragma once

// Core 1, at each Amiga frame's start (Host.c): while the RTG card's
// monitor switch is on, refreshes its picture on the HDMI output and
// returns 1 (draw no Amiga frame); returns -1 once when it goes off (the
// Amiga's frame buffers were overwritten), else 0.
int rtgFrame(void);

// Host.c: feeds the HDMI audio output (call during long work on core 1).
void hostAudioPoll(void);
