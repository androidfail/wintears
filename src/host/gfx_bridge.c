/* Graphics entry points of the SDK (upstream runtime/gfx_bridge.c and the nvnBootstrapLoader override in
   third_party/d2r_gfx/gfx/nvn/hle_nvn.c). nv::SetGraphicsAllocator, nv::InitializeGraphics and nvnBootstrapLoader are
   taken over, so the NVN driver inside sdk (and its nvdrv service) never runs: every NVN function the game looks up
   is a stub into the host's NVN layer (src/gfx/nvn.c). The allocators are kept for the GPU backend. */
#include "host/hos.h"

typedef struct { uint64_t alloc, free, realloc, user; } nv_allocator;   /* guest function pointers */
static nv_allocator g_alloc, g_devtools_alloc;

static void graphics_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_alloc = (nv_allocator){a, f, r, u}; }
static void graphics_dev_allocator(uint64_t a, uint64_t f, uint64_t r, uint64_t u) { g_devtools_alloc = (nv_allocator){a, f, r, u}; }
static void graphics_initialize(uint64_t mem, uint64_t size) {
    if (!mem || !size) die("invalid graphics work memory");
    LOG("nv::InitializeGraphics: %llu KiB of graphics work memory", (unsigned long long)(size >> 10));
}

/* nvnBootstrapLoader is imported by main (defined in sdk). Unknown names give NULL, as upstream. */
static uint64_t bootstrap_loader(const char *name) { return nvn_stub(name); }

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
