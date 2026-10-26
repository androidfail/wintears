/* Write-protect dirty-page tracking of guest memory: Windows port of upstream nvn_dirty.c (nvn_dirty.h unchanged).
   Pool pages of textures the CPU writes and the GPU reads are made read-only after an upload; the game's first write
   faults, marks the page with a new generation and makes it writable again. Before a texture is used, its pages'
   generations are compared with the upload's stamp. Upstream's design and state rules are kept as they are:
     set read-only: NONE -> BUSY -> protect(read) -> PROT; unprotect: PROT -> BUSY -> protect(read/write) -> NONE.
   A page in state NONE is always writable, one in PROT always read-only; BUSY lasts one VirtualProtect.
   Changes: mprotect -> pal_protect, the fault hook is pal_fault_add (a vectored handler, first in line, so it runs
   before dynarmic's handler for JIT code), the host pre-write hook is called from dm_guest_write_prepare (svc.c),
   time is pal_now_ns. The fault path only uses atomics and VirtualProtect: no allocation, no locks. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "host/hos.h"
#include "nvn_dirty.h"

enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
#define MAX_RANGE 16384   /* upstream: about 3,900 live pools in the world */
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t latest; } range_t;
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg = 4096, g_shift = 12;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps, g_adds_total, g_mp_calls, g_mp_pages, g_mp_ns;
static _Atomic int g_live, g_live_peak, g_overflow_logged, g_busy_red;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;

void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }

static void mp(uintptr_t a, size_t len, unsigned prot) {
    uint64_t t = pal_now_ns();
    if (pal_protect(a, len, prot)) die("dirty tracking: VirtualProtect %#llx+%#zx failed", (unsigned long long)a, len);
    atomic_fetch_add(&g_mp_ns, pal_now_ns() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

/* Wait for the page to leave BUSY: 1 = it did, 0 = waited past the limit (logged once). */
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = pal_now_ns(); if (!t0) t0 = now;
        if (now - t0 > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("dirty tracking: page %p stayed busy for over %d ms; not waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0;
        }
        pal_yield();
    }
    return 1;
}
static range_t *find(uintptr_t a) {
    static _Thread_local range_t *recent[8]; static _Thread_local unsigned next;
    for (int k = 0; k < 8; k++) {
        range_t *l = recent[k];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) return l;
    }
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) { recent[next++ & 7] = &g_r[i]; return &g_r[i]; }
    return NULL;
}
static void unprotect_page(range_t *r, uintptr_t p) {   /* the caller moved the state PROT -> BUSY */
    mp(r->base + (p << g_shift), g_pg, PAL_PROT_READ | PAL_PROT_WRITE);
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
    atomic_store(&r->gen[p], g);
    while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    atomic_fetch_add(&g_now, 1);   /* upstream #90: "now" moves on again after the generation is stored */
    atomic_store(&r->state[p], PG_NONE);
}
static int on_fault(uintptr_t addr, int kind, void *os_context, void *user) {
    (void)os_context; (void)user;
    range_t *r = kind == PAL_FAULT_WRITE ? find(addr) : NULL;
    if (!r) return 0;
    uintptr_t p = (addr - r->base) >> g_shift;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1); return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);
    return atomic_load(&r->gen[p]) != 0;   /* just unprotected by another thread: retry; a page never protected is not ours */
}
/* The host is about to write guest memory (file reads go through the system and would fail on a read-only page). */
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            for (;;) {
                uint8_t st = PG_PROT;
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_preps, 1); break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break;
            }
        }
        a = stop;
    }
}
static void dirty_init(void) {
    g_pg = pal_page_size(); g_shift = (uintptr_t)__builtin_ctzll(g_pg);
    if (pal_fault_add(on_fault, NULL)) die("dirty tracking: fault handler");
    dm_guest_write_prep_register(prep_write);
}

/* Writes on behalf of the GPU (a rendering written back into CPU-visible memory): not a CPU write. */
typedef struct { range_t *r; uintptr_t p0, p1; uint8_t *was; } gpu_write_tok;
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len) return NULL;
    range_t *r = find((uintptr_t)addr); if (!r || (uintptr_t)addr + len > r->end) return NULL;
    uintptr_t a = (uintptr_t)addr, p0 = (a - r->base) >> g_shift, p1 = (a + len - 1 - r->base) >> g_shift;
    gpu_write_tok *t = malloc(sizeof *t); uint8_t *was = calloc(p1 - p0 + 1, 1);
    if (!t || !was) { free(t); free(was); return NULL; }
    *t = (gpu_write_tok){r, p0, p1, was};
    for (uintptr_t p = p0; p <= p1; p++) {
        for (;;) {
            uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { mp(r->base + (p << g_shift), g_pg, PAL_PROT_READ | PAL_PROT_WRITE); was[p - p0] = 1; break; }
            if (st != PG_BUSY || !busy_wait(r, p)) break;
        }
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    for (uintptr_t p = t->p0; p <= t->p1; p++) if (t->was[p - t->p0]) { mp(t->r->base + (p << g_shift), g_pg, PAL_PROT_READ); atomic_store(&t->r->state[p], PG_PROT); }
    free(t->was); free(t);
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, dirty_init);
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr), slot = -1;
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    /* upstream: a pool over memory another live range covers gets no second range (find returns the first) */
    for (int i = 0; i < n; i++) {
        if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire) || end <= g_r[i].base || base >= g_r[i].end) continue;
        pthread_mutex_unlock(&g_slot_mu); return;
    }
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            if (!atomic_exchange(&g_overflow_logged, 1)) LOG("dirty tracking: all %d ranges in use, pool %p is not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state; _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("dirty tracking: page table for %zu pages", np);
        r->cap = np;   /* the old arrays are not freed: a fault handler may still read them for a moment */
    } else { memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); }
    r->base = base; r->end = end; r->state = state; r->gen = gen; atomic_store(&r->latest, 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) { range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end; }
static const char *g_no_protect;   /* why write protection is off by default (nvn_dirty_no_protect, vk_dev.c) */
void nvn_dirty_no_protect(const char *why) { g_no_protect = why; }   /* before any pool: content checks instead */
int nvn_dirty_protecting(void) { static int on = -1; if (on < 0) { const char *e = pal_getenv("TOTK_DIRTY_PROTECT"); on = e ? *e != '0' : !pal_under_wine() && !g_no_protect; } return on; }
void nvn_dirty_protect(const void *addr, size_t len) {
    /* TOTK_DIRTY_PROTECT=0: pages are never made read-only (diagnosis: an AMD driver under Proton may refuse imported
       memory with read-only pages). Textures then never see later CPU writes. */
    static int on = -1; if (on < 0) { on = nvn_dirty_protecting(); const char *e = pal_getenv("TOTK_DIRTY_PROTECT");
        if (!on) LOG("dirty tracking by write protection: off (%s); textures are checked by content instead", e ? "TOTK_DIRTY_PROTECT=0" : g_no_protect ? g_no_protect : "Wine / Proton: the Linux GPU driver refuses imported memory whose protection changes"); }
    if (!on) return;
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift, run = 0;
    int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {   /* runs of pages taken NONE -> BUSY are protected with one call */
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PAL_PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   /* this page again */
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   /* untracked: always dirty (upload too often rather than show stale content) */
    range_t *r = find((uintptr_t)addr);
    if (atomic_load(&r->latest) <= stamp) return 0;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;
    return 0;
}
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) { (void)lo; (void)hi; (void)out; (void)max; return 0; }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) { *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ns); }
