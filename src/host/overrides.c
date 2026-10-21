/* Host overrides of SDK functions (upstream runtime_override / runtime_observer, which bind the game's imports to
   host functions at link time). On the JIT track an import cannot point at host code: it points at a bridge stub
   (svc #0x1000+n; ret), whose thunk passes the guest's x0..x7 to the host function as Windows x64 arguments and puts
   the result back in x0.

   That covers what upstream's overrides take: integers and pointers (guest pointers are host pointers here), up to
   eight of them, returning nothing, an integer of 8, 32 or 64 bits, or a bool. A function with floating-point
   arguments or a by-value struct needs a thunk of its own (generated ones exist for NVN, src/bridge). */
#include "host/hos.h"

typedef uint64_t (*fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*fn8v)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define ARGS(x) x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]

/* Extra arguments are harmless on Windows x64: the caller owns the argument area. */
static void t_void(jit_context *c, void *fn) { ((fn8v)fn)(ARGS(c->x)); }
static void t_u64(jit_context *c, void *fn) { c->x[0] = ((fn8)fn)(ARGS(c->x)); }
static void t_u32(jit_context *c, void *fn) { c->x[0] = (uint32_t)((fn8)fn)(ARGS(c->x)); }
static void t_u8(jit_context *c, void *fn) { c->x[0] = (uint8_t)((fn8)fn)(ARGS(c->x)); }

static bridge_thunk thunk_of(int ret) {
    switch (ret) {
    case HOST_RET_VOID: return t_void;
    case HOST_RET_U8: return t_u8;
    case HOST_RET_U32: return t_u32;
    default: return t_u64;
    }
}

/* One stub per name, however many relocations import it. */
static struct { const char *name; uint64_t stub; } bound[1024];
static unsigned nbound;

uint64_t host_override(const char *name, unsigned kind) {
    const host_fn *f = host_service_override(name);
    if (!f && kind == LDR_R_JUMP_SLOT) f = host_service_observer(name);   /* observers: calls only, as upstream */
    if (!f) return 0;
    for (unsigned i = 0; i < nbound; i++) if (!strcmp(bound[i].name, f->name)) return bound[i].stub;
    if (nbound == sizeof bound / sizeof *bound) die("too many host overrides");
    uint64_t stub = bridge_add(host_bridge(), f->name, thunk_of(f->ret), f->fn);
    if (!stub) die("bridge stub page full");
    bound[nbound].name = f->name;
    bound[nbound++].stub = stub;
    return stub;
}
