/* The Horizon host: what the game's SDK expects from the Switch kernel and system services, answered in this process.
   Ported from upstream games/totk/runtime (threads.c, services.c, ...) for the JIT track: guest code runs in
   dynarmic, so every guest thread is a host thread that runs its registers on the jit_core of the virtual core it
   holds, and an svc returns to the host instead of jumping into a gate.

   Order of use (src/app/main.c):
     host_memory_init()            reserve the guest regions (code, stack, heap, alias), make the runtime page
     host_bridge()                 the stub page for host functions (also for the loader's resolver)
     ... map and link the modules into [host_code_base(), + host_code_size()) ...
     host_init(mods)               threads, virtual cores and their JIT cores
     host_thread_main_init(&ctx)   this thread becomes the guest main thread; ctx gets its stack and TLS
     host_guest_call(entry, ...)   run guest code on it */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "bridge/bridge.h"
#include "jit/jit.h"
#include "loader/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

int host_memory_init(char *err, size_t cap);
uint64_t host_code_base(void);
uint64_t host_code_size(void);
bridge *host_bridge(void);

void host_init(ldr_set *mods);
/* The loader's resolver, phase 0: the guest address of the host function that replaces this import (a bridge stub),
   or 0 to keep the game's own. */
uint64_t host_override(const char *name, unsigned kind);
void host_set_trace(int svcs);

/* Registers the calling host thread as the guest main thread (priority 44, core 0, cores 0-2) and returns its
   handle; fills ctx with the main stack (in the stack region) and the thread's TLS. */
uint32_t host_thread_main_init(jit_context *ctx);
/* Run guest fn(args) on the calling guest thread until it returns; returns x0. */
uint64_t host_guest_call(uint64_t fn, int n, const uint64_t *args);

#ifdef __cplusplus
}
#endif
