// HDMI audio path (RP2350 builds with OMEGA_HDMI_AUDIO): Paula's 48 kHz
// output from core 0, time-stretched on core 1 to real time (WSOLA, so the
// pitch stays right when the emulation runs slower than a real Amiga), and
// handed to the scanout's line interrupt.
#ifndef HDMI_AUDIO_H
#define HDMI_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Core 0: queues `frames` stereo samples (left/right interleaved; NULL for
// silence).
void hdmi_audio_put(const int16_t *samples, int frames);
// Core 1 thread: stretches queued input into the output ring; call often
// (every few hundred microseconds at most).
void hdmi_audio_service(void);
// Output samples ready for hdmi_audio_take().
int hdmi_audio_available(void);
// The stretcher holds no sound (silence, or not started): its output is
// silence until hdmi_audio_available() says otherwise.
bool hdmi_audio_idle(void);
// Core 1 line interrupt: the next output sample (silence if none).
void hdmi_audio_take(int16_t *lr);

#endif
