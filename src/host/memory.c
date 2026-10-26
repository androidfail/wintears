/* Guest memory regions and the memory svcs. Ported from upstream games/totk/runtime/services.c (layout, GetInfo,
   SetHeapSize, QueryMemory, Map/UnmapPhysicalMemory, Map/UnmapMemory, Break, OutputDebugString) and loader.c (module
   permissions, module queries). Windows pages are 4 KiB like the guest's, so upstream's 16 KiB rounding and its
   stale-page bookkeeping are gone: decommitted pages come back zero-filled.

   Layout (same offsets as upstream, so GetInfo answers match):
     low region, 6 GiB, the first free GiB from 0x3_0000_0000 (ASLR region = low .. 2^39):
       +0x0000_0000  code: the modules (placeholder reservation, the loader commits them)
       +0x0fc0_0000  runtime page, 4 MiB: bridge stubs, return trap, user exception context, TLS blocks
       +0x1000_0000  stack region, 256 MiB (MapMemory targets; the main thread's stack at its top)
       +0x2000_0000  host arena, 256 MiB: host data the game reads (shared fonts); upstream mallocs it, which on
                     Windows can land above the 2^39 guest address space
       +0x3000_0000  heap, up to 4 GiB (SetHeapSize)
     alias region, 64 GiB at 0x70_0000_0000 (MapPhysicalMemory), or next to the low region if that is taken */
#define _CRT_RAND_S   /* rand_s */
#include <stdlib.h>

#include "host/hos.h"

#define GUEST_ADDRESS_LIMIT (1ull << 39)
#define ALIAS_SPACE_AT 0x7000000000ull
#define ALIAS_SIZE_MAX (64ull << 30)
#define ALIAS_GRAIN (16ull << 20)
#define LOW_SEARCH_FROM 0x300000000ull
#define LOW_SIZE (6ull << 30)
#define CODE_SIZE 0x0fc00000ull
#define RUNTIME_OFF 0x0fc00000ull
#define RUNTIME_SIZE 0x00400000ull
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (4ull << 30)
#define ARENA_OFF (STACK_OFF + STACK_SIZE)
#define ARENA_SIZE (HEAP_OFF - ARENA_OFF)
#define APP_POOL 0xcd500000ull     /* retail 3.2 GiB application pool (upstream app_pool without TOTK_OUTPUT_RES) */
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define GAME_PROGRAM_ID 0x0100f2c0115b6000ull
#define MAIN_STACK_SIZE (8ull << 20)

/* runtime page */
#define RT_STUBS 0x0
#define RT_STUBS_SIZE 0x10000
#define RT_TRAP 0x10000
#define RT_EXCEPTION 0x11000
#define RT_OBJECTS 0x12000
#define RT_TLS 0x20000
#define TLS_STRIDE 0x3000
#define TLS_COUNT ((RUNTIME_SIZE - RT_TLS) / TLS_STRIDE)

static uint64_t low, alias_start, alias_size = ALIAS_SIZE_MAX, heap, heap_size, alias_bytes, arena_used;
static bridge *stubs;
static pal_mutex memory_lock = PAL_MUTEX_INIT;
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[4096];
static unsigned nstack_maps;
static unsigned char alias_pages[ALIAS_SIZE_MAX / 4096];
static unsigned char tls_used[TLS_COUNT];

#define ALIAS_START alias_start
static void (*g_heap_shrink)(uint64_t lo, uint64_t hi), (*g_alias_unmap)(uint64_t lo, uint64_t hi);
void host_on_alias_unmap(void (*fn)(uint64_t lo, uint64_t hi)) { g_alias_unmap = fn; }
void host_alias_region(uint64_t *base, uint64_t *size) { *base = ALIAS_START; *size = alias_size; }
/* The alias region is committed in grains of ALIAS_GRAIN from its start: a grain stays committed (and readable) while
   any page in it is mapped; pages unmapped in a grain still in use stay as they are and come back zeroed. The GPU is given
   whole grains (Wine / Proton): TOTK keeps its physical memory as hundreds of separate runs of about 1 MiB, one GPU
   import each at page granularity, and every import is named in every submission (750 imports: 12 ms each). */
static uint32_t alias_grain_pages[ALIAS_SIZE_MAX / ALIAS_GRAIN];   /* mapped pages per grain */
int host_alias_committed(uint64_t lo, uint64_t hi) {
    if (lo < ALIAS_START || hi > alias_start + alias_size || hi <= lo) return 0;
    pal_mutex_lock(&memory_lock);
    int ok = 1;
    for (uint64_t g = (lo - ALIAS_START) / ALIAS_GRAIN; ok && g <= (hi - 1 - ALIAS_START) / ALIAS_GRAIN; g++) ok = alias_grain_pages[g] != 0;
    pal_mutex_unlock(&memory_lock);
    return ok;
}
void host_on_heap_shrink(void (*fn)(uint64_t lo, uint64_t hi)) { g_heap_shrink = fn; }
void host_guest_layout(uint64_t *low_base, uint64_t *low_size, uint64_t *heap_cap) { *low_base = low; *low_size = LOW_SIZE; *heap_cap = HEAP_CAP; }
void host_heap_range(uint64_t *base, uint64_t *size) {
    pal_mutex_lock(&memory_lock); *base = heap; *size = heap_size; pal_mutex_unlock(&memory_lock);
}
#define ALIAS_SIZE alias_size
#define STACK_START (low + STACK_OFF)
#define ASLR_START low
#define ASLR_SIZE (GUEST_ADDRESS_LIMIT - low)

uint64_t host_code_base(void) { return low; }
uint64_t host_code_size(void) { return CODE_SIZE; }
bridge *host_bridge(void) { return stubs; }
uint64_t host_runtime_page(void) { return low + RUNTIME_OFF; }
uint64_t host_return_trap(void) { return low + RUNTIME_OFF + RT_TRAP; }
uint64_t host_object_page(void) { return low + RUNTIME_OFF + RT_OBJECTS; }
uint64_t host_main_stack(uint64_t *size) { *size = MAIN_STACK_SIZE; return low + STACK_OFF + STACK_SIZE - MAIN_STACK_SIZE; }

static void release(uint64_t at, uint64_t size) { if (size) pal_unreserve((uintptr_t)at, (size_t)size); }

/* The low region in four reservations: code (placeholders: modules are committed as private pieces), then the stack
   region, the gap and the heap as dynamic regions (page ranges committed and given back at will). */
static int reserve_low(uint64_t at) {
    if (pal_reserve_fixed((uintptr_t)at, CODE_SIZE + RUNTIME_SIZE)) return -1;
    if (pal_reserve_dynamic((uintptr_t)(at + STACK_OFF), STACK_SIZE)) { release(at, CODE_SIZE + RUNTIME_SIZE); return -1; }
    if (pal_reserve_dynamic((uintptr_t)(at + STACK_OFF + STACK_SIZE), HEAP_OFF - STACK_OFF - STACK_SIZE)) {
        release(at, CODE_SIZE + RUNTIME_SIZE); release(at + STACK_OFF, STACK_SIZE); return -1;
    }
    if (pal_reserve_dynamic((uintptr_t)(at + HEAP_OFF), LOW_SIZE - HEAP_OFF)) {
        release(at, CODE_SIZE + RUNTIME_SIZE); release(at + STACK_OFF, STACK_SIZE);
        release(at + STACK_OFF + STACK_SIZE, HEAP_OFF - STACK_OFF - STACK_SIZE); return -1;
    }
    return 0;
}

int host_memory_init(char *err, size_t cap) {
    if (!pal_reserve_dynamic(ALIAS_SPACE_AT, ALIAS_SIZE_MAX)) alias_start = ALIAS_SPACE_AT;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= ALIAS_SPACE_AT && !low; at += 1ull << 30)
        if (!reserve_low(at)) low = at;
    if (!low) { snprintf(err, cap, "cannot reserve the guest's low region (6 GiB) below 0x70_0000_0000"); return -1; }
    if (!alias_start) {
        /* as upstream on newer macOS: the alias region right after the low region, shrinking until it fits */
        for (uint64_t g = ALIAS_SIZE_MAX; g >= (8ull << 30) && !alias_start; g -= 4ull << 30)
            if (!pal_reserve_dynamic((uintptr_t)(low + LOW_SIZE), g)) { alias_start = low + LOW_SIZE; alias_size = g; }
        if (!alias_start) { snprintf(err, cap, "cannot reserve the guest's alias region below 2^39"); return -1; }
    }
    heap = low + HEAP_OFF;
    uint64_t rt = low + RUNTIME_OFF, stack = host_main_stack(&(uint64_t){0});
    if (pal_commit_private((uintptr_t)rt, RUNTIME_SIZE, PAL_PROT_READ | PAL_PROT_WRITE) ||
        pal_commit((uintptr_t)stack, MAIN_STACK_SIZE, PAL_PROT_READ | PAL_PROT_WRITE)) {
        snprintf(err, cap, "cannot commit the runtime page or the main thread's stack");
        return -1;
    }
    uint32_t trap = 0xd4000001u | HOST_RETURN_SVC << 5;
    memcpy((void *)(uintptr_t)(rt + RT_TRAP), &trap, 4);
    stubs = bridge_create((uintptr_t)(rt + RT_STUBS), RT_STUBS_SIZE);
    LOG("guest layout: low %#llx (code, stack %#llx+%#llx, heap %#llx cap %#llx), alias %#llx+%#llx",
        (unsigned long long)low, (unsigned long long)STACK_START, (unsigned long long)STACK_SIZE,
        (unsigned long long)heap, (unsigned long long)HEAP_CAP, (unsigned long long)ALIAS_START,
        (unsigned long long)ALIAS_SIZE);
    return stubs ? 0 : -1;
}

void *host_guest_alloc(size_t size) {
    uint64_t n = ((uint64_t)size + 4095) & ~4095ull;
    pal_mutex_lock(&memory_lock);
    if (!n || n > ARENA_SIZE - arena_used) die("host arena full (%llu of %llu bytes used, %zu more asked)",
                                               (unsigned long long)arena_used, (unsigned long long)ARENA_SIZE, size);
    uint64_t at = low + ARENA_OFF + arena_used;
    arena_used += n;
    pal_mutex_unlock(&memory_lock);
    if (pal_commit((uintptr_t)at, (size_t)n, PAL_PROT_READ | PAL_PROT_WRITE)) die("host arena commit");
    return (void *)(uintptr_t)at;
}

uint64_t host_tls_alloc(void) {
    pal_mutex_lock(&memory_lock);
    for (unsigned i = 0; i < TLS_COUNT; i++) {
        if (tls_used[i]) continue;
        tls_used[i] = 1;
        pal_mutex_unlock(&memory_lock);
        uint64_t tls = low + RUNTIME_OFF + RT_TLS + (uint64_t)i * TLS_STRIDE;
        memset((void *)(uintptr_t)tls, 0, TLS_STRIDE);
        /* as upstream runtime_thread_init: TLR+0x1f8 points to a ThreadType stand-in until nn::os sets its own */
        uint64_t thread_type = tls + 0x200;
        memcpy((void *)(uintptr_t)(tls + 0x1f8), &thread_type, 8);
        return tls;
    }
    pal_mutex_unlock(&memory_lock);
    die("out of guest TLS blocks (%u threads)", (unsigned)TLS_COUNT);
}

void host_tls_free(uint64_t tls) {
    uint64_t i = (tls - low - RUNTIME_OFF - RT_TLS) / TLS_STRIDE;
    pal_mutex_lock(&memory_lock);
    if (i < TLS_COUNT) tls_used[i] = 0;
    pal_mutex_unlock(&memory_lock);
}

static uint64_t image_bytes(void) {
    uint64_t total = 0;
    for (unsigned i = 0; i < ldr_count(host_mods); i++) total += ldr_module_at(host_mods, i)->size;
    return total;
}

/* ---- module permissions (upstream loader.c): text r-x, rodata r--, data rw- with RELRO changes per page ------- */

static unsigned char *module_permissions[16];

static unsigned module_page_permission(unsigned i, uint64_t off) {
    const ldr_module *m = ldr_module_at(host_mods, i);
    if (off < m->data_off) return off < m->ro_off ? 5 : 1;
    return module_permissions[i] ? module_permissions[i][(off - m->data_off) / 4096] : 3;
}

static void permission_table(unsigned i, const ldr_module *m) {
    if (module_permissions[i]) return;
    size_t count = (m->size - m->data_off + 4095) / 4096;
    module_permissions[i] = malloc(count);
    if (!module_permissions[i]) die("module permission table");
    memset(module_permissions[i], 3, count);
}

static uint64_t module_permission(uint64_t address, uint64_t size, unsigned permission) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (permission != 0 && permission != 1 && permission != 3) return 0xd801;
    for (unsigned i = 0; i < ldr_count(host_mods); i++) {
        const ldr_module *m = ldr_module_at(host_mods, i);
        if (address < m->base || address - m->base < m->data_off || address - m->base >= m->size ||
            size > m->size - (address - m->base))
            continue;
        uint64_t off = address - m->base;
        pal_mutex_lock(&memory_lock);
        permission_table(i, m);
        for (uint64_t p = 0; p < size / 4096; p++)
            if (module_permissions[i][(off - m->data_off) / 4096 + p] & 16) { pal_mutex_unlock(&memory_lock); return 0xd401; }
        memset(module_permissions[i] + (off - m->data_off) / 4096, (int)permission, size / 4096);
        unsigned prot = (permission & 1 ? PAL_PROT_READ : 0) | (permission & 2 ? PAL_PROT_WRITE : 0);
        if (pal_protect((uintptr_t)address, (size_t)size, prot)) die("module data permission");
        pal_mutex_unlock(&memory_lock);
        LOG("%s data permission +%#llx size=%#llx perm=%u", m->name, (unsigned long long)off, (unsigned long long)size,
            permission);
        return 0;
    }
    return 0xd401;
}

static uint64_t module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (mask != 16 || attribute != 16) return 0xd801;   /* the SDK only uses the irreversible RELRO lock */
    for (unsigned i = 0; i < ldr_count(host_mods); i++) {
        const ldr_module *m = ldr_module_at(host_mods, i);
        if (address < m->base || address - m->base < m->data_off || address - m->base >= m->size ||
            size > m->size - (address - m->base))
            continue;
        pal_mutex_lock(&memory_lock);
        permission_table(i, m);
        size_t first = (address - m->base - m->data_off) / 4096;
        for (uint64_t p = 0; p < size / 4096; p++) module_permissions[i][first + p] |= 16;
        pal_mutex_unlock(&memory_lock);
        return 0;
    }
    return 0xd401;
}

static int module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *perm,
                        unsigned *attr) {
    for (unsigned i = 0; i < ldr_count(host_mods); i++) {
        const ldr_module *m = ldr_module_at(host_mods, i);
        if (address < m->base || address - m->base >= m->size) continue;
        uint64_t off = (address - m->base) & ~4095ull, lo = off, hi = off + 4096;
        pal_mutex_lock(&memory_lock);
        unsigned p = module_page_permission(i, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_permission(i, lo - 4096) == p) lo -= 4096;
        while (hi < end && module_page_permission(i, hi) == p) hi += 4096;
        pal_mutex_unlock(&memory_lock);
        *base = m->base + lo; *size = hi - lo; *state = data ? 4 : 3; *perm = p & 7; *attr = p & 16;
        return 1;
    }
    return 0;
}

/* ---- MapMemory / UnmapMemory: stacks moved into the stack region (copies, as upstream) ----------------------- */

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }

static uint64_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t size) {
    if (!size || ((dst | src | size) & 4095)) return 0xcc01;
    if (dst < STACK_START || size > STACK_SIZE || dst - STACK_START > STACK_SIZE - size) return 0xdc01;
    int in_heap = src >= heap && size <= heap_size && src - heap <= heap_size - size;
    int in_alias = src >= ALIAS_START && size <= ALIAS_SIZE && src - ALIAS_START <= ALIAS_SIZE - size;
    if (!in_heap && !in_alias) return 0xdc01;
    pal_mutex_lock(&memory_lock);
    if (in_alias) {
        uint64_t first = (src - ALIAS_START) / 4096;
        for (uint64_t i = 0; i < size / 4096; i++)
            if (!alias_pages[first + i]) { pal_mutex_unlock(&memory_lock); return 0xdc01; }
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == size) found = i;
    if (unmap) {
        if (found == nstack_maps) { pal_mutex_unlock(&memory_lock); return 0xd401; }
        if (pal_protect((uintptr_t)src, (size_t)size, PAL_PROT_READ | PAL_PROT_WRITE)) die("restore moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, size);
        stack_maps[found] = stack_maps[--nstack_maps];
        if (pal_decommit((uintptr_t)dst, (size_t)size)) die("decommit moved stack");
    } else {
        if (nstack_maps == 4096) { pal_mutex_unlock(&memory_lock); return 0xd401; }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, size, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, size, stack_maps[i].src, stack_maps[i].size)) {
                pal_mutex_unlock(&memory_lock);
                return 0xdc01;
            }
        if (pal_commit((uintptr_t)dst, (size_t)size, PAL_PROT_READ | PAL_PROT_WRITE)) die("commit moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
        /* The source stays accessible (Horizon makes it inaccessible; the game never touches it). The guest heap is
           imported for the GPU as one range, and Linux's GPU drivers (the Steam Deck under Proton) check every page of
           an imported range at each submission: one inaccessible page made the first submission fail (device lost). */
        stack_maps[nstack_maps++] = (StackMap){dst, src, size};
    }
    pal_mutex_unlock(&memory_lock);
    return 0;
}

/* ---- MapPhysicalMemory / UnmapPhysicalMemory: the alias region, page by page ---------------------------------- */

static uint64_t alias_map(int unmap, uint64_t addr, uint64_t size) {
    if (!size || ((addr | size) & 4095)) return 0xcc01;
    if (addr < ALIAS_START || size > ALIAS_SIZE || addr - ALIAS_START > ALIAS_SIZE - size) return 0xdc01;
    pal_mutex_lock(&memory_lock);
    uint64_t first = (addr - ALIAS_START) / 4096, count = size / 4096;
    /* Horizon maps only the still unmapped pages of the range and keeps mapped pages and their contents: TOTK's
       physical-memory heaps grow by mapping ranges that overlap what they own. */
    uint64_t fresh = 0;
    if (!unmap) for (uint64_t i = 0; i < count; i++) if (!alias_pages[first + i]) fresh += 4096;
    /* LimitReached (132), which the SDK turns into its recoverable out-of-memory result */
    if (!unmap && fresh + alias_bytes + heap_size + image_bytes() > APP_POOL) { pal_mutex_unlock(&memory_lock); return 0x10801; }
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(addr, size, stack_maps[i].src, stack_maps[i].size)) { pal_mutex_unlock(&memory_lock); return 0xdc01; }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++)
            if (alias_pages[first + i]) { alias_pages[first + i] = 0; alias_grain_pages[(first + i) * 4096 / ALIAS_GRAIN]--; released += 4096; }
        alias_bytes -= released;
        uint64_t g0 = (addr - ALIAS_START) / ALIAS_GRAIN, g1 = (addr + size - 1 - ALIAS_START) / ALIAS_GRAIN, e0 = UINT64_MAX, e1 = 0;
        for (uint64_t g = g0; g <= g1; g++) if (!alias_grain_pages[g]) { if (e0 == UINT64_MAX) e0 = g; e1 = g; }
        /* Grains left empty go: the GPU lets go of them first (outside the lock: it waits for the GPU). They are marked
           empty before, so no new GPU window can be made over them meanwhile: a window over decommitted pages lost the
           device at the next submission on the Steam Deck. */
        if (e0 != UINT64_MAX && g_alias_unmap) {
            pal_mutex_unlock(&memory_lock);
            g_alias_unmap(ALIAS_START + e0 * ALIAS_GRAIN, ALIAS_START + (e1 + 1) * ALIAS_GRAIN);
            pal_mutex_lock(&memory_lock);
        }
        for (uint64_t g = g0; g <= g1; g++) {
            uint64_t glo = ALIAS_START + g * ALIAS_GRAIN, lo = addr > glo ? addr : glo, hi = addr + size < glo + ALIAS_GRAIN ? addr + size : glo + ALIAS_GRAIN;
            /* a grain still in use keeps the pages as they are (readable: a GPU window may hold it); they are zeroed
               when mapped again */
            (void)lo; (void)hi;
            if (!alias_grain_pages[g] && pal_decommit((uintptr_t)glo, (size_t)ALIAS_GRAIN)) die("decommit physical alias pages");
        }
    } else if (fresh) {
        for (uint64_t g = (addr - ALIAS_START) / ALIAS_GRAIN; g <= (addr + size - 1 - ALIAS_START) / ALIAS_GRAIN; g++) {
            uint64_t glo = ALIAS_START + g * ALIAS_GRAIN;
            uint64_t p0 = (addr > glo ? addr : glo) - ALIAS_START, p1 = (addr + size < glo + ALIAS_GRAIN ? addr + size : glo + ALIAS_GRAIN) - ALIAS_START;
            int was = alias_grain_pages[g] != 0;   /* else committed now: zero-filled */
            if (!was && pal_commit((uintptr_t)glo, (size_t)ALIAS_GRAIN, PAL_PROT_READ | PAL_PROT_WRITE)) die("commit physical alias pages");
            for (uint64_t p = p0 / 4096; p < p1 / 4096; p++)
                if (!alias_pages[p]) {
                    alias_pages[p] = 1; alias_grain_pages[g]++;
                    if (was) memset((void *)(uintptr_t)(ALIAS_START + p * 4096), 0, 4096);   /* discarded pages read back as anything */
                }
        }
        alias_bytes += fresh;
    }
    pal_mutex_unlock(&memory_lock);
    return 0;
}

/* ---- svcs ------------------------------------------------------------------------------------------------------ */

static uint64_t random64(void) {
    unsigned a = 0, b = 0;
    rand_s(&a);
    rand_s(&b);
    return (uint64_t)a << 32 | b;
}

static void query_memory(uint64_t *r) {
    uint64_t addr = r[2], base = addr & ~0xfffull, size = 0x1000, main_size;
    unsigned state = 0, perm = 0, attr = 0;
    uint64_t main_stack = host_main_stack(&main_size), rt = low + RUNTIME_OFF;
    if (addr >= main_stack && addr < main_stack + main_size) { base = main_stack; size = main_size; state = 0xb; perm = 3; }
    else if (module_query(addr, &base, &size, &state, &perm, &attr)) {}
    else if (addr >= rt && addr < rt + RUNTIME_SIZE) { state = 3; perm = 5; }    /* host-owned (upstream: host memory) */
    else if (addr >= heap && addr < heap + heap_size) { base = heap; size = heap_size; state = 5; perm = 3; }
    else if (addr >= low + ARENA_OFF && addr < low + ARENA_OFF + ARENA_SIZE) { base = low + ARENA_OFF; size = ARENA_SIZE; state = 3; perm = 3; }
    else if (addr >= low && addr < low + LOW_SIZE) {
        if (addr < heap) { base = low; size = ARENA_OFF; }
        else { base = heap + heap_size; size = low + LOW_SIZE - base; }
    }
    else if (addr >= low + LOW_SIZE && addr < ALIAS_START) { base = low + LOW_SIZE; size = ALIAS_START - base; state = 0x10; }
    else if (!addr) { base = 0; size = ASLR_START; }
    else { state = 3; perm = 5; }
    pal_mutex_lock(&memory_lock);
    if (addr >= ALIAS_START && addr < ALIAS_START + ALIAS_SIZE) {
        uint64_t p = (addr - ALIAS_START) / 4096, lo = p, hi = p + 1, pages = ALIAS_SIZE / 4096;
        unsigned allocated = alias_pages[p];
        while (lo > 0 && alias_pages[lo - 1] == allocated) lo--;
        while (hi < pages && alias_pages[hi] == allocated) hi++;
        base = ALIAS_START + lo * 4096; size = (hi - lo) * 4096; state = allocated ? 5 : 0; perm = allocated ? 3 : 0;
    }
    for (unsigned i = 0; i < nstack_maps; i++) {
        StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (addr >= m->dst && addr < end) { base = m->dst; size = m->size; state = 0xb; perm = 3; break; }
        if (addr >= m->src && addr < send) { base = m->src; size = m->size; state = 5; perm = 0; attr = 4; break; }
        if (state == 0) {
            uint64_t old_end = base + size;
            if (end <= addr && end > base) base = end;
            if (m->dst > addr && m->dst < old_end) old_end = m->dst;
            size = old_end - base;
        } else if (state == 5) {
            uint64_t old_end = base + size;
            if (send <= addr && send > base) base = send;
            if (m->src > addr && m->src < old_end) old_end = m->src;
            size = old_end - base;
        }
    }
    pal_mutex_unlock(&memory_lock);
    struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info = {base, size, state, attr, perm, 0, 0, 0};
    if (r[0]) memcpy((void *)(uintptr_t)r[0], &info, sizeof info);
    r[0] = 0;
    r[1] = 0;
}

int memory_svc(unsigned num, uint64_t *r) {
    switch (num) {
    case 0x02: r[0] = module_permission(r[0], r[1], (unsigned)r[2]); return 1;
    case 0x03: r[0] = module_lock_permission(r[0], r[1], (unsigned)r[2], (unsigned)r[3]); return 1;
    case 0x29: {   /* GetInfo(x1 id) */
        uint64_t value = 0;
        unsigned id = (unsigned)r[1];
        switch (id) {
        case 0: value = 7; break;                                   /* core mask */
        case 1: value = 0x0ffffffff0000000ull; break;               /* priority mask */
        case 2: value = ALIAS_START; break;
        case 3: value = ALIAS_SIZE; break;
        case 4: value = heap; break;
        case 5: value = HEAP_CAP; break;
        case 6: case 21: value = APP_POOL; break;
        case 7: case 22: value = image_bytes() + heap_size + alias_bytes; break;
        case 8: case 10: value = 0; break;
        case 11: value = random64(); break;
        case 12: value = ASLR_START; break;
        case 13: value = ASLR_SIZE; break;
        case 14: value = STACK_START; break;
        case 15: value = STACK_SIZE; break;
        case 16: value = SYSTEM_RESOURCE_BUDGET; break;
        case 17: value = sizeof stack_maps + sizeof alias_pages; break;
        case 18: value = GAME_PROGRAM_ID; break;
        case 20: value = low + RUNTIME_OFF + RT_EXCEPTION; break;     /* UserExceptionContextAddress */
        case 23: value = 1; break;
        case 28: value = 0; break;                                  /* AliasRegionExtraSize */
        default: die("GetInfo id %u not implemented", id);
        }
        r[0] = 0;
        r[1] = value;
        return 1;
    }
    case 0x2c: case 0x2d: {
        uint64_t a = r[0], n = r[1];
        r[0] = alias_map(num == 0x2d, a, n);
        if (r[0]) LOG("%sMapPhysicalMemory %#llx+%#llx failed %#llx (alias %#llx used, heap %#llx)", num == 0x2d ? "Un" : "",
                      (unsigned long long)a, (unsigned long long)n, (unsigned long long)r[0],
                      (unsigned long long)alias_bytes, (unsigned long long)heap_size);
        return 1;
    }
    case 0x04: case 0x05: r[0] = stack_move(num == 5, r[0], r[1], r[2]); return 1;
    case 0x06: query_memory(r); return 1;
    case 0x01: {   /* SetHeapSize(x1 size) */
        uint64_t size = r[1];
        if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > APP_POOL) { r[0] = 0xd401; return 1; }
        /* Shrinking: the heap ends at the new size first (no new GPU window past it), then the GPU lets go of the rest
           (outside the lock: it waits for the GPU), then it is decommitted. */
        pal_mutex_lock(&memory_lock);
        uint64_t old = heap_size;
        if (size < old) heap_size = size;
        pal_mutex_unlock(&memory_lock);
        if (size < old && g_heap_shrink) g_heap_shrink(heap + size, heap + old);
        pal_mutex_lock(&memory_lock);
        if (size > old && pal_commit((uintptr_t)(heap + old), (size_t)(size - old), PAL_PROT_READ | PAL_PROT_WRITE))
            die("commit heap");
        if (size < old && pal_decommit((uintptr_t)(heap + size), (size_t)(old - size))) die("decommit heap");
        heap_size = size;
        pal_mutex_unlock(&memory_lock);
        r[0] = 0;
        r[1] = heap;
        return 1;
    }
    case 0x26: {   /* Break(x0 reason, x1 info, x2 size) */
        uint32_t result = 0;
        if (r[1] && r[2] >= 4) memcpy(&result, (void *)(uintptr_t)r[1], 4);
        LOG("guest Break reason=%llx result=%x", (unsigned long long)r[0], result);
        if (r[0] & 0x80000000ull) { r[0] = 0; return 1; }   /* notification only */
        return 0;   /* reported by the caller with the guest's registers */
    }
    case 0x27: {   /* OutputDebugString(x0 text, x1 size) */
        size_t n = r[1];
        if (n > 65536) die("debug string too long");
        fwrite((void *)(uintptr_t)r[0], 1, n, stderr);
        r[0] = 0;
        return 1;
    }
    }
    return 0;
}
