/* nn::vi: the game draws into its own NVN swapchain textures and the GPU backend shows them in the window
   (gfx/vk/vk_present.c). Upstream (runtime/frontend.m) overrides the same
   five functions: Initialize, OpenDefaultDisplay, CreateLayer, GetNativeWindow and GetDisplayVsyncEvent. The display,
   the layer and the native window are tokens in guest memory the game only passes back; the vsync event is a real
   SystemEvent, signalled 60 times a second by a host thread, as the window's display link does upstream. */
#include <stdatomic.h>
#include <pthread.h>

#include "host/hos.h"

#define VSYNC_MAX 16
static uint64_t vsync_events[VSYNC_MAX];
static int vsync_count, vi_ready;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t display_token(void) { return host_object_page(); }
static uint64_t layer_token(void) { return host_object_page() + 0x40; }
static uint64_t window_token(void) { return host_object_page() + 0x80; }

_Atomic unsigned long long vi_n_vsync;   /* vsync signals (the 10-second log) */
static void *vsync_thread(void *unused) {
    (void)unused;
    uint64_t next = pal_now_ns();
    for (;;) {
        next += 16666667;
        uint64_t now = pal_now_ns();
        if (next > now) pal_sleep_ns(next - now);
        else next = now;   /* fell behind (debugger, load): no burst of catch-up signals */
        pthread_mutex_lock(&vsync_lock);
        int n = vsync_count;
        uint64_t ev[VSYNC_MAX];
        memcpy(ev, vsync_events, sizeof ev);
        pthread_mutex_unlock(&vsync_lock);
        for (int i = 0; i < n; i++) hle_signal_system_event(ev[i]);
        { extern _Atomic unsigned long long vi_n_vsync; vi_n_vsync++; }
    }
    return NULL;
}

static void vi_initialize(void) {
    vi_ready = 1;
    LOG("VI display initialized");
}
static uint32_t vi_open(uint64_t *out) {
    if (!vi_ready || !out) return 0xf601;
    *out = display_token();
    return 0;
}
static uint32_t vi_layer(uint64_t *out, uint64_t display) {
    if (!out || display != display_token()) return 0xf601;
    *out = layer_token();
    return 0;
}
static uint32_t vi_window(uint64_t *out, uint64_t layer) {
    if (!out || layer != layer_token()) return 0xf601;
    *out = window_token();
    return 0;
}
static uint32_t vi_vsync(uint64_t ev, uint64_t display) {
    if (!ev || display != display_token()) return 0xf601;
    hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&vsync_lock);
    if (vsync_count >= VSYNC_MAX) die("VI display event limit");
    vsync_events[vsync_count++] = ev;
    int first = vsync_count == 1;
    pthread_mutex_unlock(&vsync_lock);
    if (first) {
        pthread_t t;
        if (pthread_create(&t, NULL, vsync_thread, NULL)) die("vsync thread");
        pthread_detach(t);
        LOG("display vsync: 60 Hz timer");
    }
    return 0;
}

static const host_fn table[] = {
    {"_ZN2nn2vi10InitializeEv", (void *)vi_initialize, HOST_RET_VOID},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", (void *)vi_open, HOST_RET_U32},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", (void *)vi_layer, HOST_RET_U32},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", (void *)vi_window, HOST_RET_U32},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", (void *)vi_vsync, HOST_RET_U32},
};
const host_fn *host_vi_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}
