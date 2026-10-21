/* The audio device (Windows: WASAPI, shared mode). The mixer (audio_mixer.c) renders into a ring; the device's thread
   pulls from it whenever the device wants frames, as upstream's CoreAudio render callback does. */
#pragma once
#include <stddef.h>

typedef struct audio_out audio_out;
typedef void (*audio_out_pull)(void *ctx, float *lr, unsigned frames);   /* interleaved stereo float frames */

/* The default output device at rate Hz, stereo float (Windows converts to the device's format). NULL with a reason in
   why when there is no device; the mixer then runs without sound. */
audio_out *audio_out_open(int rate, audio_out_pull pull, void *ctx, char *why, size_t cap);
void audio_out_close(audio_out *o);
