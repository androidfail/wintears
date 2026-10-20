#include "bridge/bridge.h"

#include <stdlib.h>
#include <string.h>

#include "pal.h"

struct bridge {
    uint32_t *stubs;          /* guest stub page: two instructions per entry */
    unsigned cap, count;
    struct { const char *name; bridge_thunk thunk; void *fn; } *e;
    pal_mutex lock;
};

bridge *bridge_create(uintptr_t stub_page, size_t page_bytes) {
    bridge *b = (bridge *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->stubs = (uint32_t *)stub_page;
    b->cap = (unsigned)(page_bytes / 8);
    if (b->cap > BRIDGE_SVC_MAX - BRIDGE_SVC_BASE + 1) b->cap = BRIDGE_SVC_MAX - BRIDGE_SVC_BASE + 1;
    b->e = calloc(b->cap, sizeof *b->e);
    if (!b->e) { free(b); return NULL; }
    return b;
}

uint64_t bridge_add(bridge *b, const char *name, bridge_thunk thunk, void *fn) {
    pal_mutex_lock(&b->lock);
    unsigned i = b->count;
    uint64_t at = 0;
    if (i < b->cap && thunk) {
        b->e[i].name = name; b->e[i].thunk = thunk; b->e[i].fn = fn;
        uint32_t imm = BRIDGE_SVC_BASE + i;
        b->stubs[2 * i] = 0xd4000001u | imm << 5;   /* svc #imm */
        b->stubs[2 * i + 1] = 0xd65f03c0u;          /* ret */
        b->count = i + 1;
        at = (uint64_t)(uintptr_t)&b->stubs[2 * i];
    }
    pal_mutex_unlock(&b->lock);
    return at;
}

int bridge_dispatch(bridge *b, uint32_t svc, jit_context *ctx) {
    if (svc < BRIDGE_SVC_BASE || svc - BRIDGE_SVC_BASE >= b->count) return 0;
    unsigned i = svc - BRIDGE_SVC_BASE;
    b->e[i].thunk(ctx, b->e[i].fn);
    return 1;
}

const char *bridge_name(bridge *b, uint32_t svc) {
    if (svc < BRIDGE_SVC_BASE || svc - BRIDGE_SVC_BASE >= b->count) return NULL;
    return b->e[svc - BRIDGE_SVC_BASE].name;
}

static int cmp(const void *k, const void *e) { return strcmp((const char *)k, ((const bridge_entry *)e)->name); }
bridge_thunk bridge_nvn_thunk(const char *name) {
    const bridge_entry *e = (const bridge_entry *)bsearch(name, bridge_nvn_table, bridge_nvn_count, sizeof *e, cmp);
    return e ? e->thunk : NULL;
}
