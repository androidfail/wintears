/* Horizon kernel threads and synchronization, for the JIT track. Ported from upstream games/totk/runtime/threads.c
   (itself adapted from D2R Mobile host/hle_thread.c, MIT), TOTK paths only.

   Each guest thread is a host pthread (winpthreads). Upstream runs the guest code natively on the guest stack; here
   the thread runs its registers in dynarmic on the jit_core of the virtual core it holds (one per core, 3 cores).
   A guest thread only gives its core up inside an svc, outside the JIT, so the core's JIT state is never shared.
   What upstream does with signals is a dynarmic halt here: a better thread that queues for a core halts the owner's
   jit_core, and the owner gives the core up at the halt as at an svc (runtime_core_checkpoint).

   The svc semantics are upstream's, which follow the public Horizon kernel behaviour (Atmosphere mesosphere's
   KConditionVariable / KAddressArbiter / KSynchronization):
     threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority
                0x0E/0x0F core mask 0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
     locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
     condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
     address    0x34 WaitForAddress 0x35 SignalToAddress
     events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization
                0x16 CloseHandle
   All waits hang on one global lock G with one condition variable per thread. */
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "host/hos.h"

#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u
#define NCORE 3

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   /* O_SESSION / O_TMEM: IPC objects (ipc.c) */
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top, tls; int prio, core; uint64_t mask;
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    /* current wait: kind 0 = none, 1 = lock, 2 = condition, 3 = address, 4 = synchronization object */
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             /* wait registration order: at equal priority first come, first woken */
    struct thr *next;          /* wait chain */
    struct thr *all;           /* all threads */
    int last_svc; uint64_t last_lr, last_fp;
    int closed;                /* handle closed: freed when it has also exited */
    uint64_t nsvc;
    int cur_core;              /* virtual core held while running guest code, -1 while blocked or not started */
    volatile int preempt;      /* a better thread waits for this thread's core */
    uint64_t cseq, core_since; struct thr *cnext;
    uint32_t wh[4]; int nwh;
    jit_context *ctx;          /* the registers the thread runs now (nested host-to-guest calls replace it) */
    void *osh;                 /* host thread handle for the sampler (never closed: the sampler may still hold it) */
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
/* Where guest threads wait (the 10-second log, runtime_wait_report): G itself, a free core, guest waits; svcs. */
static _Atomic uint64_t g_ns_lock, g_ns_core, g_ns_block, g_n_lock_slow, g_n_core_wait, g_n_block, g_n_preempt;
/* Core time: free (no owner), handed over but the new owner not running yet, running guest code; the rest is spent
   in svcs and host calls while holding the core. */
static _Atomic uint64_t g_ns_free, g_ns_handoff, g_ns_jit;
static uint64_t g_free_since[3];
/* Calls from guest code: count and time per svc (index < 0x100) and per host stub (0x100 + stub index); the reasons
   jit_run returned. */
enum { NCALL = 0x100 + 8192 };
static _Atomic uint64_t g_call_n[NCALL], g_call_ns[NCALL], g_n_run, g_n_stop_halt, g_n_stop_wait;
static inline int call_slot(uint32_t n) { return n < 0x100 ? (int)n : n >= BRIDGE_SVC_BASE && n - BRIDGE_SVC_BASE < NCALL - 0x100 ? (int)(0x100 + n - BRIDGE_SVC_BASE) : -1; }
/* The timers below (every jit_run, svc, host call, wait: several clock reads and shared counters each) run only with
   TOTK_DIAG=1 (the [calls] and waits lines). */
static int g_diag;
static inline void call_note(uint32_t n, uint64_t t0) { int i = call_slot(n); if (i >= 0) { atomic_fetch_add(&g_call_n[i], 1); atomic_fetch_add(&g_call_ns[i], pal_now_ns() - t0); } }
static void G_lock(void) {
    if (!pthread_mutex_trylock(&G)) return;
    uint64_t t0 = pal_now_ns(); pthread_mutex_lock(&G);
    atomic_fetch_add(&g_ns_lock, pal_now_ns() - t0); atomic_fetch_add(&g_n_lock_slow, 1);
}
void runtime_wait_report(char *out, size_t cap) {
    static uint64_t last; uint64_t now = pal_now_ns(), span = last ? now - last : 0; last = now;
    double fr = atomic_exchange(&g_ns_free, 0) / 1e6, ho = atomic_exchange(&g_ns_handoff, 0) / 1e6, jt = atomic_exchange(&g_ns_jit, 0) / 1e6, all = 3 * span / 1e6;
    snprintf(out, cap, "lock G waited %.0f ms (%llu times), waiting for a core %.0f ms (%llu times, %llu preemptions), guest waits %.0f ms (%llu); "
             "of %.0f core-ms: guest code %.0f, free %.0f, handing over %.0f, svcs and host calls %.0f",
             atomic_exchange(&g_ns_lock, 0) / 1e6, (unsigned long long)atomic_exchange(&g_n_lock_slow, 0), atomic_exchange(&g_ns_core, 0) / 1e6,
             (unsigned long long)atomic_exchange(&g_n_core_wait, 0), (unsigned long long)atomic_exchange(&g_n_preempt, 0),
             atomic_exchange(&g_ns_block, 0) / 1e6, (unsigned long long)atomic_exchange(&g_n_block, 0), all, jt, fr, ho, all - jt - fr - ho);
}
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
pthread_key_t runtime_keys[8];
static int g_key_ok;   /* winpthreads' first key is 0, so the key value itself cannot say whether it exists */
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          /* every waiting thread (lock / condition / address / synchronization) */

static jit_monitor *g_monitor;
static jit_core *g_jit[NCORE];

static uint32_t h_new(int type, void *obj) {   /* holding G */
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("handle table full");
}
static thr_t *me_or_null(void);
static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return me_or_null();
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) LOG("invalid handle %#x (wanted type %d)", h, type);
    return o;
}
static thr_t *me_or_null(void) { return g_key_ok ? pthread_getspecific(g_keythr) : NULL; }
static thr_t *cur(void) {
    thr_t *t = me_or_null();
    if (!t) die("thread svc from a host thread that is not a guest thread");
    return t;
}

/* ---------- wait primitives (holding G) ---------- */
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
/* Kernel order: lower priority value first, then first registered. kind 1 by lock address, 2 by condition key, 3 by
   address. */
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
static int64_t block_inner(thr_t *t, int kind, int64_t ns);

/* ---------- virtual cores ----------
   Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
   GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they run
   guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking host
   services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a better
   waiter wants to preempt is halted in the JIT and gives its core up there (upstream: at its next svc). On the JIT
   track the cores are always on: a core is also the jit_core the thread runs on. */
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }
static void core_grant(int c) {   /* holding G: give free core c to the best waiter */
    thr_t *b = NULL, **bp = NULL;
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) { b = *p; bp = p; }
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); g_core_owner[c] = b; pthread_cond_signal(&b->cv);
}
static void core_put(thr_t *t) {   /* holding G */
    if (t->cur_core < 0) return;
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL; core_grant(c);
    if (!g_core_owner[c]) g_free_since[c] = now_ns();
}
/* t waits for a core: halt the one owner it may displace (the worst priority worse than t's, not already asked). */
static void preempt_for(const thr_t *t) {
    int v = -1;
    for (int i = 0; i < NCORE; i++) {
        thr_t *o = g_core_owner[i];
        if (core_allowed(t, i) && o && o->prio > t->prio && !o->preempt && (v < 0 || o->prio > g_core_owner[v]->prio)) v = i;
    }
    if (v >= 0) { g_core_owner[v]->preempt = 1; jit_halt(g_jit[v]); }   /* the owner stops at its next block (run, JIT_STOP_HALT) */
}
static void core_take(thr_t *t) {   /* holding G; waits until a core in t's mask is handed over */
    if (t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) {
        t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t;
        if (g_free_since[c]) atomic_fetch_add(&g_ns_free, t->core_since - g_free_since[c]);
        g_free_since[c] = 0; return;
    }
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t;
    preempt_for(t);
    uint64_t t0 = g_diag ? pal_now_ns() : 0;
    while (t->cur_core < 0) pthread_cond_wait(&t->cv, &G);
    if (!g_diag) return;
    uint64_t t1 = pal_now_ns();
    atomic_fetch_add(&g_ns_core, t1 - t0); atomic_fetch_add(&g_n_core_wait, 1);
    if (t1 > t->core_since) atomic_fetch_add(&g_ns_handoff, t1 - t->core_since);
}
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
/* Host services that wait for other threads give the core up around the wait (no host locks held). */
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    G_lock(); core_put(t); pthread_mutex_unlock(&G); return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    G_lock(); core_take(t); pthread_mutex_unlock(&G);
}
/* Preemption point at svc entry and at a JIT halt: a better thread waits for this core. */
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null();
    if (!t || !t->preempt) return;
    G_lock(); t->preempt = 0;
    /* The waiter that asked may have got another core meanwhile: hand this one over only to a better thread, never
       to a worse one that the owner would then displace again. */
    int better = 0;
    for (thr_t *w = g_core_wait; w && !better; w = w->cnext) better = core_allowed(w, t->cur_core) && w->prio < t->prio;
    if (better) {
        atomic_fetch_add(&g_n_preempt, 1); core_put(t);
        for (thr_t *w = g_core_wait; w; w = w->cnext) preempt_for(w);   /* better waiters still without a core */
        core_take(t);
    }
    pthread_mutex_unlock(&G);
}
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

static int64_t block(thr_t *t, int kind, int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t); int64_t r = block_inner(t, kind, ns); core_take(t); return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    if (ns > 0) {
        clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    uint64_t t0 = g_diag ? pal_now_ns() : 0; if (g_diag) atomic_fetch_add(&g_n_block, 1);
    while (!t->woke) {
        int rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke) { wait_unlink(t); t->wkind = 0; if (g_diag) atomic_fetch_add(&g_ns_block, pal_now_ns() - t0); return R_TIMEOUT; }
    }
    if (g_diag) atomic_fetch_add(&g_ns_block, pal_now_ns() - t0);
    return t->wres;
}

/* ---------- locks ---------- */
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

/* Release the lock at addr: hand it to the first thread waiting on it, or clear it (holding G) */
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
/* Make t take the lock at addr: if free it gets it and is woken, else set the waiters bit and make it a lock wait */
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;   /* still in g_wait, now a lock wait */
    t->wseq = ++g_wseq;                              /* after the lock's existing waiters of equal priority */
}

/* ---------- running guest code ---------- */
static void thread_exit(thr_t *t);

static void report_stop(const char *what, uint32_t n, jit_context *c) {
    char where[96];
    host_describe(c->pc - 4, where, sizeof where);
    thr_t *t = me_or_null();
    LOG("%s 0x%02x at %s on thread %#x", what, n, where, t ? t->handle : 0);
    host_report_guest(c);
}

/* Run c on the calling guest thread until it reaches the return trap. */
static void run(thr_t *t, jit_context *c) {
    jit_context *outer = t->ctx;
    t->ctx = c;
    for (;;) {
        if (t->cur_core < 0) die("thread %#x runs guest code without a virtual core", t->handle);
        jit_core *jc = g_jit[t->cur_core];
        jit_load(jc, c);
        uint64_t tj = g_diag ? pal_now_ns() : 0;
        int stop = jit_run(jc);
        if (g_diag) { atomic_fetch_add(&g_ns_jit, pal_now_ns() - tj); atomic_fetch_add(&g_n_run, 1); }
        jit_save(jc, c);
        if (stop == JIT_STOP_HALT) { atomic_fetch_add(&g_n_stop_halt, 1); runtime_core_checkpoint(); continue; }
        if (stop == JIT_STOP_EXCEPTION) {
            uint64_t pc;
            int kind = jit_exception(jc, &pc);
            if (kind == JIT_EXC_WAIT) {
                /* WFE / WFI (the SDK's light message queues and spin locks wait for an event with WFE): the core is
                   given up for a moment so other guest threads run, then the guest continues after the instruction,
                   as after a spurious wake-up (the code re-checks its condition). */
                atomic_fetch_add(&g_n_stop_wait, 1);
                int away = runtime_core_away();
                pal_yield();
                runtime_core_back(&away);
                continue;
            }
            uint32_t insn = 0;
            pal_guarded_copy(&insn, (const void *)(uintptr_t)pc, 4);
            char where[96];
            host_describe(pc, where, sizeof where);
            LOG("guest exception %d at %s (instruction 0x%08x) on thread %#x", kind, where, insn, t->handle);
            host_report_guest(c);
            die("guest exception");
        }
        uint32_t n = jit_svc_number(jc);
        if (n == HOST_RETURN_SVC) break;
        if (n >= BRIDGE_SVC_BASE) {
            uint64_t ts = g_diag ? pal_now_ns() : 0; int done = bridge_dispatch(host_bridge(), n, c); if (g_diag) call_note(n, ts);
            if (done) continue;
            report_stop("unknown host stub svc", n, c);
            die("unknown host stub");
        }
        t->last_svc = (int)n; t->last_lr = c->x[30]; t->last_fp = c->x[29]; t->nsvc++;
        { uint64_t ts = g_diag ? pal_now_ns() : 0; int done = host_svc(n, c); if (g_diag) call_note(n, ts); if (done) continue; }
        report_stop("unimplemented svc", n, c);
        die("svc 0x%02x is not implemented yet", n);
    }
    t->ctx = outer;
}

uint64_t host_guest_call(uint64_t fn, int n, const uint64_t *args) {
    thr_t *t = cur();
    jit_context c;
    memset(&c, 0, sizeof c);
    if (t->ctx) {
        c = *t->ctx;
        c.sp = (t->ctx->sp - 0x200) & ~15ull;   /* below the caller's frame, keeping its red zone */
    } else {
        c.sp = t->stack_top;
        c.tpidrro_el0 = t->tls;
        c.tpidr_el0 = t->tls + 0x1000;
    }
    for (int i = 0; i < 8; i++) c.x[i] = i < n ? args[i] : 0;
    c.pc = fn;
    c.x[29] = 0;
    c.x[30] = host_return_trap();
    run(t, &c);
    return c.x[0];
}

/* ---------- threads ---------- */
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_setspecific(runtime_keys[2], (void *)(uintptr_t)t->tls);
    { char nm[32]; snprintf(nm, sizeof nm, "guest %#x p%d", t->handle, t->prio); pal_thread_set_name(nm); }
    t->osh = pal_thread_handle_self();
    G_lock(); while (!t->started) pthread_cond_wait(&t->cv, &G); core_take(t); pthread_mutex_unlock(&G);
    jit_context c;
    memset(&c, 0, sizeof c);
    c.pc = t->entry;
    c.x[0] = t->arg;
    c.sp = t->stack_top;
    c.tpidrro_el0 = t->tls;
    c.tpidr_el0 = t->tls + 0x1000;
    c.x[30] = host_return_trap();
    run(t, &c);
    die("guest thread %#x returned from its entry (Horizon requires ExitThread)", t->handle);
}
static void thr_free(thr_t *t) {   /* holding G */
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static void thread_exit(thr_t *t) {
    G_lock();
    core_put(t);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   /* sync waiters look again */
    pthread_setspecific(g_keythr, NULL);
    host_tls_free(t->tls);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}

static void *sampler(void *unused);
void *volatile runtime_replay_thread;   /* set by the Vulkan backend while it replays a submission */
void threads_init(void) {
    if (pthread_key_create(&g_keythr, NULL)) die("thread key");
    for (int i = 0; i < 8; i++) if (pthread_key_create(&runtime_keys[i], NULL)) die("runtime key");
    g_key_ok = 1;
    g_monitor = jit_monitor_create(NCORE);
    jit_config cfg = {0};
    cfg.fastmem = 1;
    for (int i = 0; i < NCORE; i++) g_jit[i] = jit_core_create(&cfg, g_monitor, (size_t)i);
    { const char *d = pal_getenv("TOTK_DIAG"); g_diag = d && *d == '1'; }
    const char *e = pal_getenv("TOTK_SAMPLE");
    if (e && *e == '1') { pthread_t th; if (!pthread_create(&th, NULL, sampler, NULL)) pthread_detach(th); }
}

/* Sampling profiler (TOTK_SAMPLE=1; off by default: under Proton a stopped thread's driver call is interrupted and the
   kernel starts a GPU submission over, which made the Steam Deck crawl): 100 times a second, where each core's
   owner is (pc and the calling functions in this program); the most frequent places are logged every 10 s. */
typedef struct { int n, total; struct { char k[260]; unsigned n; } e[600]; } stab;
static void stab_add(const char *k, void *u) {
    stab *s = u; int i = 0;
    while (i < s->n && strcmp(s->e[i].k, k)) i++;
    if (i == s->n) { if (s->n == (int)(sizeof s->e / sizeof *s->e)) return; snprintf(s->e[s->n++].k, sizeof s->e[0].k, "%s", k); s->e[i].n = 0; }
    s->e[i].n++; s->total++;
}
static void stab_log(stab *s, const char *what, int show) {
    char out[4096]; size_t at = 0; int part = 0;
    for (int shown = 0; shown < show; shown++) {
        int b = -1; for (int i = 0; i < s->n; i++) if (s->e[i].n && (b < 0 || s->e[i].n > s->e[b].n)) b = i;
        if (b < 0 || s->e[b].n * 1000 < (unsigned)s->total) break;   /* under 0.1 % */
        at += (size_t)snprintf(out + at, at < sizeof out ? sizeof out - at : 0, "%s%.1f%% %s", at ? " | " : "", s->e[b].n * 100.0 / s->total, s->e[b].k);
        s->e[b].n = 0;
        if (at >= 3000) { LOG("%s (%d samples, part %d): %s", what, s->total, part++, out); at = 0; out[0] = 0; }
    }
    if (at || !part) LOG("%s (%d samples%s): %s", what, s->total, part ? ", last part" : "", at ? out : "-");
    s->n = 0; s->total = 0;
}
static void log_calls(void) {   /* the svcs and host functions guest code spent the most time in */
    char out[3000]; size_t at = 0; uint64_t ns[NCALL], n[NCALL], tn = 0;
    for (int i = 0; i < NCALL; i++) { ns[i] = atomic_exchange(&g_call_ns[i], 0); n[i] = atomic_exchange(&g_call_n[i], 0); tn += n[i]; }
    for (int shown = 0; shown < 16; shown++) {
        int b = -1; for (int i = 0; i < NCALL; i++) if (ns[i] && (b < 0 || ns[i] > ns[b])) b = i;
        if (b < 0) break;
        const char *nm = b < 0x100 ? host_svc_name((unsigned)b) : bridge_name(host_bridge(), BRIDGE_SVC_BASE + (uint32_t)(b - 0x100));
        at += (size_t)snprintf(out + at, at < sizeof out ? sizeof out - at : 0, "%s%s%s%#x %llux %.0f ms", shown ? ", " : "", nm ? nm : "", nm ? " " : "",
                               b < 0x100 ? (unsigned)b : BRIDGE_SVC_BASE + (unsigned)(b - 0x100), (unsigned long long)n[b], ns[b] / 1e6);
        ns[b] = 0;
        if (at >= sizeof out) break;
    }
    static uint64_t lb, ls; uint64_t b, sl; jit_compile_stats(&b, &sl);
    LOG("[calls] guest code ran %llu times between stops (%llu halts for preemption, %llu WFE waits), %llu blocks compiled (%llu new slabs), %llu calls; most time in: %s",
        (unsigned long long)atomic_exchange(&g_n_run, 0), (unsigned long long)atomic_exchange(&g_n_stop_halt, 0),
        (unsigned long long)atomic_exchange(&g_n_stop_wait, 0), (unsigned long long)(b - lb), (unsigned long long)(sl - ls), (unsigned long long)tn, out);
    lb = b; ls = sl;
}
static void log_threads(void) {   /* every guest thread: priority, what it is doing, its last svc */
    static const char *wk[] = {"", "lock", "condvar", "address", "sync"};
    char out[1200]; size_t at = 0; int part = 0;
    G_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        int queued = 0; for (thr_t *w = g_core_wait; w && !queued; w = w->cnext) queued = w == t;
        char st[64];
        if (t->cur_core >= 0) snprintf(st, sizeof st, "on core %d%s", t->cur_core, t->preempt ? " (asked to yield)" : "");
        else if (queued) snprintf(st, sizeof st, "wants a core");
        else if (t->wkind > 0 && t->wkind < 5) snprintf(st, sizeof st, "waits %s %#llx", wk[t->wkind], (unsigned long long)t->waddr);
        else if (!t->started) snprintf(st, sizeof st, "not started");
        else snprintf(st, sizeof st, "away (sleep or host wait)");
        at += (size_t)snprintf(out + at, at < sizeof out ? sizeof out - at : 0, "%s%#x p%d mask %llx %s, last svc %#x, %llu svcs",
                               at ? "; " : "", t->handle, t->prio, (unsigned long long)t->mask, st, t->last_svc, (unsigned long long)t->nsvc);
        if (at > 900) { LOG("[threads %d] %s", part++, out); at = 0; out[0] = 0; }
    }
    pthread_mutex_unlock(&G);
    if (at) LOG("[threads %d] %s", part, out);
}
static void *sampler(void *unused) {
    (void)unused;
    pal_thread_set_name("sampler");
    static stab cores, others, coarse, replay;
    uint64_t last = pal_now_ns(), last_dump = last; unsigned tick = 0;
    { char b[3000]; pal_linux_report(b, sizeof b); }   /* baselines */
    for (;;) {
        pal_sleep_ns(10000000); tick++;
        void *hs[NCORE]; unsigned long ids[NCORE + 1]; int nids = 0;
        G_lock();
        for (int c = 0; c < NCORE; c++) { thr_t *o = g_core_owner[c]; hs[c] = o ? o->osh : NULL; }
        pthread_mutex_unlock(&G);
        for (int c = 0; c < NCORE; c++) {
            char k[260];
            if (!hs[c]) { stab_add("(core free)", &cores); continue; }
            ids[nids++] = pal_thread_id_of(hs[c]);
            if (pal_thread_sample(hs[c], k, sizeof k)) {
                stab_add(k, &cores);
                char *second = strstr(k, " < "); if (second) second = strstr(second + 3, " < "); if (second) *second = 0;
                stab_add(k, &coarse);
            }
        }
        if (tick % 20 == 0) pal_sample_busy_threads(ids, nids, stab_add, &others);   /* 5 times a second */
        { void *r = runtime_replay_thread; char k[260]; if (r && pal_thread_sample(r, k, sizeof k)) stab_add(k, &replay); }
        uint64_t now = pal_now_ns();
        if (now - last < 10000000000ull) continue;
        if (now - last > 13000000000ull) LOG("[cores] the sampler itself was late by %.1f s", (now - last) / 1e9 - 10);
        last = now;
        stab_log(&cores, "[cores] where the core owners are", 16);
        stab_log(&others, "[other threads] where the other busy threads are", 16);
        stab_log(&replay, "[replay] where the thread replaying a submission is", 30);
        log_calls();
        char b[6000];
        pal_linux_report(b, sizeof b); LOG("[linux] %s", b);
        pal_exception_report(b, sizeof b); LOG("[exceptions] %s", b);
        if (now - last_dump >= 30000000000ull) { last_dump = now; log_threads(); }
        if (tick >= 6000) { tick = 0; stab_log(&coarse, "[cores minute] every place the core owners were in the last minute", 600); }
    }
    return NULL;
}

/* Register the main thread and return its handle (nnosInitialize takes it as the main thread handle) */
uint32_t host_thread_main_init(jit_context *ctx) {
    thr_t *t = calloc(1, sizeof *t);
    if (!t) die("thread allocation");
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->osh = pal_thread_handle_self();
    t->cur_core = -1;
    uint64_t stack_size, stack = host_main_stack(&stack_size);
    t->stack_top = stack + stack_size - 0x100;
    t->tls = host_tls_alloc();
    G_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t); pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    pthread_setspecific(runtime_keys[2], (void *)(uintptr_t)t->tls);
    memset(ctx, 0, sizeof *ctx);
    ctx->sp = t->stack_top;
    ctx->tpidrro_el0 = t->tls;
    ctx->tpidr_el0 = t->tls + 0x1000;
    return t->handle;
}

/* ---------- synchronization objects ---------- */
static int sig_obj(uint32_t h, int *bad) {   /* 1 = signaled (holding G) */
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}
static int64_t now_mono_ns(void) { return (int64_t)pal_now_ns(); }

void runtime_ipc_session_closed(void *session);
void runtime_ipc_tmem_closed(void *tmem);

/* Thread and synchronization svcs; returns 1 when handled */
int svc_thread(int num, uint64_t *regs) {
    switch (num) {
    case 0x08: {  /* CreateThread(-> w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core) */
        if (!regs[1] || !regs[3] || (regs[3] & 15) || (int)regs[4] < 28 || (int)regs[4] > 59 || (int)regs[5] < -3 || (int)regs[5] > 2) { regs[0] = R_BAD_STATE; return 1; }
        thr_t *t = calloc(1, sizeof *t);
        if (!t) die("thread allocation");
        t->cur_core = -1;
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->tls = host_tls_alloc();
        G_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);   /* the JIT and host services run on it */
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   /* the guest waits through the handle */
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx prio=%d core=%d", t->handle, (unsigned long long)t->entry,
            (unsigned long long)t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  /* StartThread(w0) */
        G_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  /* SleepThread(x0 ns); 0 / -1 / -2 = the kinds of yield */
        int64_t ns = (int64_t)regs[0];
        thr_t *self = cur();
        if (ns > 0) {
            G_lock(); core_put(self); pthread_mutex_unlock(&G);
            pal_sleep_ns((uint64_t)ns);
            G_lock(); core_take(self); pthread_mutex_unlock(&G);
        } else {   /* Horizon yield: hand the core to a waiting thread of equal or better priority */
            G_lock();
            int give = 0; for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = self->cur_core >= 0 && core_allowed(w, self->cur_core) && w->prio <= self->prio;
            if (give) { core_put(self); core_take(self); }
            pthread_mutex_unlock(&G);
            if (!give) pal_yield();
        }
        return 1;
    }
    case 0x0C: { G_lock(); thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0D: { if (regs[1] < 28 || regs[1] > 59) { regs[0] = R_BAD_STATE; return 1; } G_lock(); thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { G_lock(); thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  /* SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = process default, -2/-3 = keep the ideal core */
        G_lock(); thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   /* migrate off a core no longer allowed */
        regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }
    case 0x25: { G_lock(); thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  /* ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag) */
        thr_t *t = cur(); uint64_t addr = regs[1];
        G_lock();
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: G_lock(); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  /* WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout) */
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        G_lock();
        /* Order matters (upstream): set the condition key first, then release the mutex. The SDK's Signal reads the
           key in user mode and skips svc 0x1D when it is 0. */
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr);
        int64_t r = block(t, 2, (int64_t)regs[3]);   /* on timeout the lock is not taken again (user mode does it) */
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  /* SignalProcessWideKey(x0 condition key, w1 count, <= 0 all) */
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        G_lock();
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    case 0x34: {  /* WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if equal */
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        G_lock();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  /* SignalToAddress(x0 address, w1 type, w2 value, w3 count) */
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        G_lock();
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0; for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  /* CreateEvent(-> w1 writable end, w2 readable end) */
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        G_lock(); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  /* SignalEvent (writable) / ClearEvent (either) / ResetSignal (readable) */
        G_lock();
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("event svc %#x: invalid handle %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  /* WaitSynchronization(-> w1 index; x1 handle array, w2 count, x3 timeout) */
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)(uintptr_t)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if (n < 0 || n > 64 || (!hs && n)) { regs[0] = R_BAD_STATE; return 1; }
        int64_t deadline = ns > 0 ? now_mono_ns() + ns : 0;
        G_lock();
        t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - now_mono_ns();
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  /* CancelSynchronization(w0 thread) */
        G_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  /* CloseHandle(w0): an event goes when both ends are closed; a thread object when it also exited */
        G_lock();
        unsigned i = (uint32_t)regs[0] - 0x1000; int valid = 0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            valid = 1; event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid = 1; void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            pthread_mutex_unlock(&G);
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            G_lock();
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid = 1; thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = valid ? 0 : R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

/* ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ---------- */
uint32_t runtime_handle_new(int session, void *obj) { G_lock(); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj); pthread_mutex_unlock(&G); return h; }
void *runtime_handle_get(uint32_t h, int session) { G_lock(); void *o = h_peek(h, session ? O_SESSION : O_TMEM); pthread_mutex_unlock(&G); return o; }
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    G_lock(); uint32_t hw = h_new(O_EVENT_W, e); pthread_mutex_unlock(&G); return hw;
}
uint32_t runtime_event_readable(uint32_t hw) {
    G_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G); return hr;
}
void runtime_event_signal(uint32_t hw) {
    G_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->signaled = 1; wake_sync_waiters(); pthread_mutex_unlock(&G);
}

/* ---------- SystemEvent of the service layer (upstream threads.c) ----------
   On the Switch a service hands the client the readable end of a kernel event, and the guest waits on it through the
   SDK's nn::os. Here the SDK's own CreateSystemEvent(inter-process = true) runs on a guest thread, creating the kernel
   event through svc 0x45; the writable handle is kept, and any thread (also a host thread, such as the vsync timer)
   can then signal it. */
static struct { uint64_t ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(uint64_t ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(uint64_t ev, int autoclear) {   /* on a guest thread: it runs SDK code */
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!me_or_null()) die("hle_bind_system_event must run on a guest thread");
    uint64_t create = host_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t getw = host_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
    if (!create || !getw) die("nn::os SystemEvent functions missing");
    uint64_t args[3] = {ev, (uint64_t)autoclear, 1};
    uint32_t r = (uint32_t)host_guest_call(create, 3, args);
    if (r) die("CreateSystemEvent failed %#x", r);
    wh = (uint32_t)host_guest_call(getw, 1, &ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent binding table full");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(uint64_t ev) {   /* bound events only, from any thread */
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) die("signal of an unbound SystemEvent");
    G_lock();
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent writable handle %#x is gone", wh);
    e->signaled = 1; wake_sync_waiters();
    pthread_mutex_unlock(&G);
}
