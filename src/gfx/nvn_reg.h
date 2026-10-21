/* Registration of the host's NVN functions (upstream third_party/d2r_gfx/gfx/nvn/nvn_impl.h).

   Upstream collects them in a Mach-O section and the game calls them directly (both ARM64). Here each one registers
   itself from a constructor, and the game reaches it through a bridge stub whose thunk (src/bridge/nvn_bridge_gen.c,
   generated from upstream's own NVN(...) definitions) converts the guest's AAPCS64 arguments. A ported function must
   therefore keep upstream's exact signature: nvn.c looks the thunk up by name and stops if there is none. */
#pragma once
#include "nvn_protos.h"
#include "host/hos.h"

/* NVN entry points that wait for the GPU, the display or other threads give up the caller's virtual core for their
   duration (threads.c), as the Horizon thread would block there. */
#define NVN_BLOCKING() __attribute__((cleanup(runtime_core_back))) int core_away_ = runtime_core_away()

typedef struct { const char *name; void *fn; int extra; } nvn_impl;
void nvn_register(const nvn_impl *impl);

#define NVN_REGISTER_(name, fn, extra)                                                            \
    __attribute__((constructor)) static void reg_##name(void) {                                   \
        static const nvn_impl e = {#name, (void *)fn, extra};                                     \
        nvn_register(&e);                                                                         \
    }
/* With a prototype in nvn_protos.h: the signature is checked at compile time. */
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    NVN_REGISTER_(name, h_##name, 0)                                                              \
    static ret h_##name(__VA_ARGS__)
/* Newer interfaces without a prototype: no signature check. */
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret h_##name(__VA_ARGS__);                                                             \
    NVN_REGISTER_(name, h_##name, 0)                                                              \
    static ret h_##name(__VA_ARGS__)
/* Entry points the game asks nvnDeviceGetProcAddress / nvnBootstrapLoader for that are not in main's name table. */
#define NVN_EXTRA(name, ret, ...)                                                                 \
    static ret x_##name(__VA_ARGS__);                                                             \
    NVN_REGISTER_(name, x_##name, 1)                                                              \
    static ret x_##name(__VA_ARGS__)

/* Boolean parameters: Switch callers do not extend them, only the low 8 bits count. */
#define B(x) ((x) & 0xff)

uint64_t nvn_new_serial(void);
void *nvn_slot_fn(const char *name);   /* an implemented entry by name (NVN_EXTRA fallbacks forward to regular ones) */
uint64_t nvn_stub(const char *name);   /* the guest address to hand out for an NVN name, 0 if unknown */
