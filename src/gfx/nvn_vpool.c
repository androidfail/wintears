/* NVN physical / virtual memory pools. Port of upstream third_party/d2r_gfx/gfx/nvn/totk_vpool.c (interface
   totk_vpool.h, used unchanged), with Windows sections and placeholders in place of Mach VM remapping.

   A virtual pool is a reserved address range (below 2^39, in the guest's address space) whose 64 KiB pages alias pages
   of physical pools, so CPU and GPU writes through either view hit the same memory. A physical pool the CPU cannot
   access (CPU_NO_ACCESS: the game only reaches it through virtual mappings) is a page-file section the host commits
   and maps once (pool->host); a virtual mapping is a view of that section at the virtual page. Pages are mapped one by
   one, so every page is its own placeholder piece and any request can be applied in any order.

   Differences from upstream:
     - a CPU-visible physical pool would have to alias the game's own storage, which on Windows is private memory and
       cannot be mapped twice: not supported yet, the run stops where the game makes one (TOTK 1.0.0's is CPU_NO_ACCESS);
     - unmapped virtual pages are no-access placeholders, not fresh zero pages (the CPU and the null backend never read
       them; the Vulkan backend decides how it backs sparse holes). */
#include <pthread.h>

#include "totk_vpool.h"
#include "gfx/nvn_backend.h"
#include "host/hos.h"

#define F_CPU_NO_ACCESS NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS
#define F_PHYSICAL NVN_MEMORY_POOL_FLAGS_PHYSICAL
#define F_VIRTUAL NVN_MEMORY_POOL_FLAGS_VIRTUAL
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')
#define GUEST_LIMIT (1ull << 39)

/* Host state of a pool, found by its host address (pool_t's layout is upstream's: no room for it there). */
typedef struct {
    uint8_t *host; uint64_t span;
    pal_section *section;          /* physical: its memory */
    const pool_t **page_src;       /* virtual: per 64 KiB page, the physical pool mapped there (NULL = unmapped) */
    uint64_t *page_off;            /* virtual: per page, the offset in that physical pool */
} vstate;
#define MAX_POOLS 256
static vstate g_pools[MAX_POOLS];
static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
/* page tables and pool slots, for totk_vpool_physical (the GPU backend, under its own lock): held only around changes,
   never across a call out (g_map_mu is held across gpu_pool_add / del, which take the backend's lock) */
static pthread_mutex_t g_pt_mu = PTHREAD_MUTEX_INITIALIZER;

static vstate *state_of(const uint8_t *host) {   /* g_map_mu held */
    for (int i = 0; i < MAX_POOLS; i++) if (g_pools[i].host && g_pools[i].host == host) return &g_pools[i];
    return NULL;
}
static vstate *state_new(void) {   /* g_map_mu held */
    for (int i = 0; i < MAX_POOLS; i++) if (!g_pools[i].host) return &g_pools[i];
    die("vpool: more than %d physical / virtual pools", MAX_POOLS);
}

/* What the GPU backend sees of a virtual pool: its mapped pages, as runs of consecutive pages, each registered as a pool
   of its own. The whole reserved range cannot be imported as host memory (the GPU refuses a range with unmapped pages
   in it: probed on an RTX 4070, a reserved range of 64 KiB section views imports when every page is mapped and fails
   with a hole); a run of mapped pages is one import. A mapping request changes the runs around it: those touching
   its pages or next to them are dropped and made again, the others are left as they are. */
typedef struct { uint64_t first, count; } vrun;   /* in pages */
static int runs_of(const vstate *s, vrun **out) {   /* g_map_mu held; the caller frees *out */
    uint64_t pages = s->span / TOTK_VPOOL_PAGE; vrun *r = NULL; int cap = 0, k = 0;
    for (uint64_t p = 0; p < pages;) {
        if (!s->page_src[p]) { p++; continue; }
        uint64_t q = p; while (q < pages && s->page_src[q]) q++;
        if (k == cap) { cap = cap ? cap * 2 : 16; r = realloc(r, (size_t)cap * sizeof *r); if (!r) die("vpool: run list"); }
        r[k++] = (vrun){p, q - p}; p = q;
    }
    *out = r; return k;
}
static int near_request(const vrun *u, const totk_mapping_request *r, int count) {   /* overlaps a request's pages or touches them */
    for (int i = 0; i < count; i++) {
        uint64_t lo = (uint64_t)r[i].virtual_offset / TOTK_VPOOL_PAGE, hi = lo + r[i].size / TOTK_VPOOL_PAGE;
        if (u->first <= hi && u->first + u->count >= lo) return 1;
    }
    return 0;
}
int totk_vpool_init(pool_t *o) {
    int phys = (o->flags & F_PHYSICAL) != 0, virt = (o->flags & F_VIRTUAL) != 0;
    o->game_mem = o->mem; o->host = NULL; o->span = 0;
    if (phys == virt) { LOG("vpool: pool flags %#x must be exactly one of PHYSICAL/VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % TOTK_VPOOL_PAGE) { LOG("vpool: pool size %#llx not a multiple of 64 KiB", (unsigned long long)o->size); return 0; }
    if (virt && o->mem) { LOG("vpool: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { LOG("vpool: physical pool without storage"); return 0; }
    if (phys && !(o->flags & F_CPU_NO_ACCESS))
        die("vpool: CPU-visible physical pool (size %#llx flags %#x): aliasing the game's own memory is not supported on Windows yet",
            (unsigned long long)o->size, o->flags);
    pthread_mutex_lock(&g_map_mu); pthread_mutex_lock(&g_pt_mu);
    vstate *s = state_new();
    uint64_t span = o->size;
    if (phys) {
        s->section = pal_section_create(span);
        uintptr_t at = s->section ? pal_map_view(s->section, 0, 0, (size_t)span, PAL_PROT_READ | PAL_PROT_WRITE) : 0;
        if (!at || pal_commit(at, (size_t)span, PAL_PROT_READ | PAL_PROT_WRITE)) die("vpool: physical pool of %#llx bytes", (unsigned long long)span);
        s->host = (uint8_t *)at;
    } else {
        uintptr_t at = pal_reserve_below((size_t)span, GUEST_LIMIT);
        s->page_src = calloc((size_t)(span / TOTK_VPOOL_PAGE), sizeof *s->page_src);
        s->page_off = calloc((size_t)(span / TOTK_VPOOL_PAGE), sizeof *s->page_off);
        if (!at || !s->page_src || !s->page_off) die("vpool: virtual pool of %#llx bytes below 2^39", (unsigned long long)span);
        s->host = (uint8_t *)at;
    }
    s->span = span;
    o->host = o->mem = s->host; o->span = span;
    pthread_mutex_unlock(&g_pt_mu); pthread_mutex_unlock(&g_map_mu);
    LOG("[nvn] %s memory pool: %#llx bytes at %p", phys ? "physical" : "virtual", (unsigned long long)span, (void *)o->mem);
    return 1;
}

int totk_vpool_fini(pool_t *o) {
    if (!o->host) return 0;
    pthread_mutex_lock(&g_map_mu);
    vstate *s = state_of(o->host);
    if (!s) die("vpool: finalize of an unknown pool %p", (void *)o->host);
    if (s->section) {
        pal_unmap_view((uintptr_t)s->host);
        pal_section_destroy(s->section);   /* views in virtual pools keep the memory alive */
    } else {
        vrun *runs; int nr = runs_of(s, &runs);
        for (int i = 0; i < nr; i++) gpu_pool_del(s->host + runs[i].first * TOTK_VPOOL_PAGE);
        free(runs);
        for (uint64_t p = 0; p < s->span / TOTK_VPOOL_PAGE; p++)
            if (s->page_src[p]) pal_unmap_view((uintptr_t)(s->host + p * TOTK_VPOOL_PAGE));
        pthread_mutex_lock(&g_pt_mu); free(s->page_src); free(s->page_off); s->page_src = NULL; s->page_off = NULL; pthread_mutex_unlock(&g_pt_mu);
        if (pal_unreserve((uintptr_t)s->host, (size_t)s->span)) die("vpool: release of virtual pool %p", (void *)s->host);
    }
    pthread_mutex_lock(&g_pt_mu); memset(s, 0, sizeof *s); pthread_mutex_unlock(&g_pt_mu);
    pthread_mutex_unlock(&g_map_mu);
    o->host = NULL; o->mem = o->game_mem; o->span = 0;
    return 1;
}

static int check_request(const pool_t *v, const totk_mapping_request *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) { LOG("vpool: request %d negative/empty range", i); return 0; }
    if (((uint64_t)r->virtual_offset | (uint64_t)r->physical_offset | r->size) % TOTK_VPOOL_PAGE) {
        LOG("vpool: request %d not 64 KiB aligned (v=%#llx p=%#llx n=%#llx)", i, (unsigned long long)r->virtual_offset,
            (unsigned long long)r->physical_offset, (unsigned long long)r->size);
        return 0;
    }
    if (end < r->size || end > v->size) { LOG("vpool: request %d exceeds virtual pool (%#llx > %#llx)", i, (unsigned long long)end, (unsigned long long)v->size); return 0; }
    if (!r->physical_pool) return 1;   /* unmap */
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != POOL_MAGIC || !(p->flags & F_PHYSICAL)) { LOG("vpool: request %d source %p is not a physical pool", i, (const void *)p); return 0; }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) { LOG("vpool: request %d exceeds physical pool (%#llx > %#llx)", i, (unsigned long long)pend, (unsigned long long)p->size); return 0; }
    return 1;
}

static int apply_request(vstate *v, const totk_mapping_request *r) {   /* g_map_mu held */
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    vstate *ps = p ? state_of(p->host) : NULL;
    if (p && !ps) { LOG("vpool: physical pool %p has no host memory", (const void *)p); return 0; }
    for (uint64_t k = 0; k < r->size / TOTK_VPOOL_PAGE; k++) {
        uint64_t page = (uint64_t)r->virtual_offset / TOTK_VPOOL_PAGE + k;
        uintptr_t at = (uintptr_t)(v->host + page * TOTK_VPOOL_PAGE);
        if (v->page_src[page] && pal_unmap_view(at)) { LOG("vpool: unmap at %#llx failed", (unsigned long long)at); return 0; }
        v->page_src[page] = NULL;
        if (!p) continue;
        uint64_t off = (uint64_t)r->physical_offset + k * TOTK_VPOOL_PAGE;
        if (pal_map_view(ps->section, off, at, TOTK_VPOOL_PAGE, PAL_PROT_READ | PAL_PROT_WRITE) != at) {
            LOG("vpool: view of physical offset %#llx at %#llx failed", (unsigned long long)off, (unsigned long long)at);
            return 0;
        }
        v->page_src[page] = p; v->page_off[page] = off;
    }
    return 1;
}

/* The physical memory behind [mem, mem + size) of a virtual pool: the address of its first byte in the physical pool's
   own mapping when every page is mapped to consecutive pages of one physical pool; 0 otherwise (not in a virtual pool,
   a hole, or scattered pages). The GPU sees physical pages, so a texture written through one virtual address is read
   through another that maps the same pages (the menus' scene snapshot, in TOTK's transient pool), and a remapped
   address is new memory. */
uint64_t totk_vpool_physical(const void *mem, uint64_t size) {
    uint64_t r = 0;
    pthread_mutex_lock(&g_pt_mu);
    for (int i = 0; i < MAX_POOLS; i++) {
        vstate *s = &g_pools[i];
        if (!s->host || !s->page_src || (const uint8_t *)mem < s->host || (const uint8_t *)mem >= s->host + s->span) continue;
        uint64_t at = (uint64_t)((const uint8_t *)mem - s->host), p0 = at / TOTK_VPOOL_PAGE, p1 = (at + (size ? size : 1) - 1) / TOTK_VPOOL_PAGE;
        if (p1 >= s->span / TOTK_VPOOL_PAGE || !s->page_src[p0]) break;
        int ok = 1;
        for (uint64_t p = p0 + 1; ok && p <= p1; p++) ok = s->page_src[p] == s->page_src[p0] && s->page_off[p] == s->page_off[p0] + (p - p0) * TOTK_VPOOL_PAGE;
        vstate *ps = ok ? state_of(s->page_src[p0]->host) : NULL;
        if (ps) r = (uint64_t)(uintptr_t)(ps->host + s->page_off[p0] + at % TOTK_VPOOL_PAGE);
        break;
    }
    pthread_mutex_unlock(&g_pt_mu);
    return r;
}

/* The physical pool whose own mapping holds address phys (as totk_vpool_physical returns): its range. 0 if none. */
int totk_vpool_phys_pool(uint64_t phys, uint64_t *lo, uint64_t *span) {
    int found = 0;
    pthread_mutex_lock(&g_pt_mu);
    for (int i = 0; i < MAX_POOLS && !found; i++) {
        vstate *s = &g_pools[i];
        if (!s->host || !s->section || phys < (uint64_t)(uintptr_t)s->host || phys >= (uint64_t)(uintptr_t)s->host + s->span) continue;
        *lo = (uint64_t)(uintptr_t)s->host; *span = s->span; found = 1;
    }
    pthread_mutex_unlock(&g_pt_mu);
    return found;
}

int totk_vpool_map(pool_t *v, int count, const totk_mapping_request *r) {
    if (!v || v->magic != POOL_MAGIC || !(v->flags & F_VIRTUAL) || !v->mem) { LOG("vpool: MapVirtual target %p is not a virtual pool", (void *)v); return 0; }
    if (count < 0 || (count && !r)) { LOG("vpool: invalid request count %d", count); return 0; }
    for (int i = 0; i < count; i++) if (!check_request(v, &r[i], i)) return 0;
    pthread_mutex_lock(&g_map_mu);
    vstate *s = state_of(v->host);
    if (!s || !s->page_src) die("vpool: virtual pool %p has no host range", (void *)v->mem);
    vrun *old, *now; int nold = runs_of(s, &old), nnow;
    for (int i = 0; i < nold; i++) if (near_request(&old[i], r, count)) gpu_pool_del(v->mem + old[i].first * TOTK_VPOOL_PAGE);   /* the backend's view of the old pages */
    free(old);
    int ok = 1;
    pthread_mutex_lock(&g_pt_mu);
    for (int i = 0; ok && i < count; i++) ok = apply_request(s, &r[i]);
    pthread_mutex_unlock(&g_pt_mu);
    nnow = runs_of(s, &now);
    for (int i = 0; i < nnow; i++) if (near_request(&now[i], r, count)) gpu_pool_add(v->mem + now[i].first * TOTK_VPOOL_PAGE, now[i].count * TOTK_VPOOL_PAGE);
    free(now);
    pthread_mutex_unlock(&g_map_mu);
    if (!ok) die("vpool: mapping failed after validation; virtual pool %p is inconsistent", (void *)v);
    return 1;
}
