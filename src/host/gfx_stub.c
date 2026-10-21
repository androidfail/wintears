/* Graphics entry points until the Vulkan backend exists (phase 5). Upstream (runtime/gfx_bridge.c and
   third_party/d2r_gfx/gfx/nvn/hle_nvn.c) overrides nv::SetGraphicsAllocator, nv::InitializeGraphics and
   nvnBootstrapLoader, so the NVN driver inside sdk (and its nvdrv service) never runs; these stand-ins take the same
   imports. The allocators are kept for the backend; nvnBootstrapLoader hands out one stub per NVN function, and the
   first NVN function the game calls stops the run and is named: the phase 3A boot probe ends there. */
#include "host/hos.h"

typedef struct { uint64_t alloc, free, realloc, user; } nv_allocator;   /* guest function pointers */
static nv_allocator g_alloc, g_devtools_alloc;

static void graphics_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_alloc = (nv_allocator){a, f, r, u}; }
static void graphics_dev_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_devtools_alloc = (nv_allocator){a, f, r, u}; }
static void graphics_initialize(uint64_t mem, uint64_t size) {
    if (!mem || !size) die("invalid graphics work memory");
    LOG("nv::InitializeGraphics: %llu KiB of graphics work memory (no GPU backend yet: phase 5)", (unsigned long long)(size >> 10));
}

static void nvn_called(jit_context *c, void *name) {
    LOG("first NVN call: %s (the NVN layer is the Vulkan backend's work, phase 5)", (const char *)name);
    host_report_guest(c);
    die("stopped at the first NVN call");
}

static struct { char name[128]; uint64_t stub; } looked_up[1024];
static unsigned nlooked;
static pal_mutex lookup_lock = PAL_MUTEX_INIT;

static uint64_t bootstrap_loader(const char *name) {
    if (!name) return 0;
    pal_mutex_lock(&lookup_lock);
    for (unsigned i = 0; i < nlooked; i++) if (!strcmp(looked_up[i].name, name)) { pal_mutex_unlock(&lookup_lock); return looked_up[i].stub; }
    if (nlooked == sizeof looked_up / sizeof *looked_up) die("too many NVN lookups");
    snprintf(looked_up[nlooked].name, sizeof looked_up[nlooked].name, "%s", name);
    uint64_t stub = bridge_add(host_bridge(), looked_up[nlooked].name, nvn_called, looked_up[nlooked].name);
    looked_up[nlooked++].stub = stub;
    pal_mutex_unlock(&lookup_lock);
    LOG("nvnBootstrapLoader(%s)", name);
    return stub;
}

static const host_fn table[] = {
    {"_ZN2nv18InitializeGraphicsEPvm", (void *)graphics_initialize, HOST_RET_VOID},
    {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", (void *)graphics_allocator, HOST_RET_VOID},
    {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", (void *)graphics_dev_allocator, HOST_RET_VOID},
    {"nvnBootstrapLoader", (void *)bootstrap_loader, HOST_RET_U64},
};
const host_fn *host_gfx_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}
