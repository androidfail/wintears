/* Audio renderer and mixer test (no game, no audio device): upstream's own probe (audio_renderer.c
   runtime_audio_probe) on the Windows build. It builds renderer state and RequestUpdate wire requests and checks the
   mixed samples against independent references: REV15 parsing, float and Q14 biquads, PCM16 mixing, pitch
   interpolation, DSP-ADPCM, splitters and submixes, reverb, auxiliary rings, the limiter and voice lifetime. A failed
   check stops the process with a non-zero exit. */
void runtime_audio_probe(void);

int main(void) {
    runtime_audio_probe();
    return 0;
}
