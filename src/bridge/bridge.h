/* Guest-to-host bridge: the game (ARM64, in the JIT) calls host functions (x86-64, Windows x64 convention).

   Each host function the game can reach gets a guest stub `svc #(BRIDGE_SVC_BASE + index); ret` in a stub page.
   The svc stops the JIT, the host runs the thunk for that index (which converts the arguments, calls the function
   and writes the result into the guest registers), and the guest continues at the `ret`. Thunks are generated
   from the host functions' signatures (tools/gen_bridge.py). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "jit/jit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_SVC_BASE 0x1000u   /* Horizon svcs are below 0x80 and MNG's host gates below 0x100 */
#define BRIDGE_SVC_MAX 0xFFFFu

typedef void (*bridge_thunk)(jit_context *c, void *fn);
typedef struct { const char *name; bridge_thunk thunk; } bridge_entry;

/* A page of guest stubs, at a guest address (inside guest memory, below 2^39). */
typedef struct bridge bridge;
bridge *bridge_create(uintptr_t stub_page, size_t page_bytes);
/* Register a host function; returns the guest address of its stub (the value the game should call), 0 if full. */
uint64_t bridge_add(bridge *b, const char *name, bridge_thunk thunk, void *fn);
/* For a stopped core: if svc is a bridge call, run it on ctx and return 1. */
int bridge_dispatch(bridge *b, uint32_t svc, jit_context *ctx);
const char *bridge_name(bridge *b, uint32_t svc);

/* Generated NVN thunks (sorted by name). */
extern const bridge_entry bridge_nvn_table[];
extern const unsigned bridge_nvn_count;
bridge_thunk bridge_nvn_thunk(const char *name);

#ifdef __cplusplus
}
#endif
