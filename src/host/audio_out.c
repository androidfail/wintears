/* WASAPI output (audio_out.h): shared mode, event driven, the stream at the game's rate as stereo float32 with
   Windows' converter (AUTOCONVERTPCM) taking it to the device's own format. One thread per output owns the COM
   objects: it opens the default device, then fills each device period by pulling from the mixer. When the device goes
   away (unplugged, default changed, AUDCLNT_E_DEVICE_INVALIDATED) it opens the new default one. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

#include "host/audio_out.h"

DEFINE_GUID(WT_CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(WT_IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(WT_IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(WT_IID_IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
DEFINE_GUID(WT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

struct audio_out {
    int rate; audio_out_pull pull; void *ctx;
    HANDLE thread, ready, period; volatile LONG stop; int ok; char why[160];
    IAudioClient *client; IAudioRenderClient *render; UINT32 frames;
};

static void release(audio_out *o) {
    if (o->client) IAudioClient_Stop(o->client);
    if (o->render) IAudioRenderClient_Release(o->render);
    if (o->client) IAudioClient_Release(o->client);
    o->render = NULL; o->client = NULL;
}

static int open_device(audio_out *o) {
    IMMDeviceEnumerator *e = NULL; IMMDevice *d = NULL;
    HRESULT hr = CoCreateInstance(&WT_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &WT_IID_IMMDeviceEnumerator, (void **)&e);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &d);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(d, &WT_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&o->client);
    if (d) IMMDevice_Release(d);
    if (e) IMMDeviceEnumerator_Release(e);
    if (FAILED(hr)) { snprintf(o->why, sizeof o->why, "no default output device (%#lx)", (unsigned long)hr); return 0; }
    WAVEFORMATEXTENSIBLE f = {0};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; f.Format.nChannels = 2; f.Format.nSamplesPerSec = (DWORD)o->rate;
    f.Format.wBitsPerSample = 32; f.Format.nBlockAlign = 8; f.Format.nAvgBytesPerSec = (DWORD)o->rate * 8;
    f.Format.cbSize = sizeof f - sizeof f.Format;
    f.Samples.wValidBitsPerSample = 32; f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; f.SubFormat = WT_SUBTYPE_IEEE_FLOAT;
    hr = IAudioClient_Initialize(o->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 100000 /* 10 ms */, 0, (WAVEFORMATEX *)&f, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(o->client, o->period);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(o->client, &o->frames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(o->client, &WT_IID_IAudioRenderClient, (void **)&o->render);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(o->client);
    if (FAILED(hr)) { snprintf(o->why, sizeof o->why, "output device setup failed (%#lx)", (unsigned long)hr); release(o); return 0; }
    return 1;
}

/* One device period: what the device has room for, from the mixer. 0 = the device is gone. */
static int fill(audio_out *o) {
    UINT32 padding = 0; BYTE *buf = NULL;
    HRESULT hr = IAudioClient_GetCurrentPadding(o->client, &padding);
    if (FAILED(hr)) return 0;
    UINT32 n = o->frames - padding;
    if (!n) return 1;
    hr = IAudioRenderClient_GetBuffer(o->render, n, &buf);
    if (FAILED(hr)) return 0;
    o->pull(o->ctx, (float *)buf, n);
    return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(o->render, n, 0));
}

static DWORD WINAPI run(void *arg) {
    audio_out *o = arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);   /* a short fill each period */
    o->ok = open_device(o);
    SetEvent(o->ready);
    if (o->ok) {
        while (!InterlockedCompareExchange(&o->stop, 0, 0)) {
            WaitForSingleObject(o->period, 200);
            if (o->client && fill(o)) continue;
            /* the device went away: the new default device, retried twice a second */
            release(o);
            fprintf(stderr, "[audio] output device lost: reopening the default device\n");
            while (!InterlockedCompareExchange(&o->stop, 0, 0) && !open_device(o)) Sleep(500);
            if (o->client) fprintf(stderr, "[audio] output device reopened\n");
        }
    }
    release(o);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

audio_out *audio_out_open(int rate, audio_out_pull pull, void *ctx, char *why, size_t cap) {
    audio_out *o = calloc(1, sizeof *o);
    if (!o) { snprintf(why, cap, "out of memory"); return NULL; }
    o->rate = rate; o->pull = pull; o->ctx = ctx;
    o->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    o->period = CreateEventW(NULL, FALSE, FALSE, NULL);
    o->thread = o->ready && o->period ? CreateThread(NULL, 0, run, o, 0, NULL) : NULL;
    if (!o->thread) { snprintf(why, cap, "audio thread"); if (o->ready) CloseHandle(o->ready); if (o->period) CloseHandle(o->period); free(o); return NULL; }
    WaitForSingleObject(o->ready, INFINITE);
    if (o->ok) return o;
    snprintf(why, cap, "%s", o->why);
    WaitForSingleObject(o->thread, INFINITE);
    CloseHandle(o->thread); CloseHandle(o->ready); CloseHandle(o->period); free(o);
    return NULL;
}

void audio_out_close(audio_out *o) {
    if (!o) return;
    InterlockedExchange(&o->stop, 1);
    SetEvent(o->period);
    WaitForSingleObject(o->thread, INFINITE);
    CloseHandle(o->thread); CloseHandle(o->ready); CloseHandle(o->period); free(o);
}
