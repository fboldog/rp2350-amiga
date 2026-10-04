//
//  Audio.h
//  Paula audio: four DMA channels, mixed to 48 kHz stereo.
//

#ifndef Audio_h
#define Audio_h

#include <stdint.h>

#define AUDIO_RATE 48000

// DMACON was written (old value of DMACONR); starts/stops channels.
void audioDmaconChanged(uint16_t old);
// The CPU wrote AUDxDAT (non-DMA playback while the channel's DMA is off).
void audioDatWritten(int channel);
// Advances the four channels by one raster line and emits the 48 kHz
// stereo samples that fall into it (called at the end of every line).
void audioLine(int slots);
// Silences all channels (reset).
void audioReset(void);

// Host hook, once per raster line: that line's `frames` stereo output
// samples (left/right interleaved; channels 0+3 left, 1+2 right), or NULL
// when all channels are off (`frames` samples of silence).
void hostAudioOut(const int16_t *samples, int frames);

#endif
