/* Shared inside the Horizon host (not for the rest of the program). Plays the part of upstream's runtime.h and
   thread_compat.h for the ported files. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host.h"
#include "pal.h"

void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void host_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) host_log(__VA_ARGS__)
#define die(...) host_fail(__VA_ARGS__)
_Noreturn void runtime_fail(const char *message);   /* upstream name */
#define now_ns pal_now_ns
#define guest_ns pal_now_ns
static inline int dm_detclock_enabled(void) { return 0; }

extern ldr_set *host_mods;
void host_describe(uint64_t addr, char *out, size_t cap);
void host_report_guest(const jit_context *c);
uint64_t host_symbol(const char *name);

/* Horizon result codes (module 1 = kernel) */
#define R_TIMEOUT     0xEA01u
#define R_CANCELLED   0xEC01u
#define R_BAD_HANDLE  0xE401u
#define R_BAD_STATE   0xFA01u
#define R_OUT_OF_RES  0xCE01u

/* memory.c */
int memory_svc(unsigned num, uint64_t *r);       /* 1 = handled */
uint64_t host_runtime_page(void);                 /* guest memory the host owns: stubs, traps, TLS blocks */
uint64_t host_tls_alloc(void);                    /* a thread's TLS block (TLR at +0, TPIDR_EL0 area at +0x1000) */
void host_tls_free(uint64_t tls);
uint64_t host_return_trap(void);                  /* svc #HOST_RETURN_SVC */
uint64_t host_main_stack(uint64_t *size);
#define HOST_RETURN_SVC 0xfffeu

/* threads.c */
int svc_thread(int num, uint64_t *regs);
void runtime_core_checkpoint(void);
int runtime_core_away(void);
void runtime_core_back(int *away);
void threads_init(void);
uint32_t runtime_handle_new(int session, void *obj);
void *runtime_handle_get(uint32_t h, int session);
uint32_t runtime_event_new(void);
uint32_t runtime_event_readable(uint32_t writable);
void runtime_event_signal(uint32_t writable);

/* upstream ipc.c (compiled unchanged) */
int runtime_ipc_svc(unsigned num, uint64_t *regs);
void runtime_trace_guest(uint64_t fp, uint64_t lr);
#include <pthread.h>
extern pthread_key_t runtime_keys[8];   /* [2]: the thread's TLR (IPC message buffer), as upstream */

/* overrides.c / services.c: host functions that replace SDK functions */
enum { HOST_RET_VOID, HOST_RET_U8, HOST_RET_U32, HOST_RET_U64 };
typedef struct { const char *name; void *fn; int ret; } host_fn;
const host_fn *host_service_override(const char *name);   /* upstream runtime_override */
const host_fn *host_service_observer(const char *name);   /* upstream runtime_observer */

/* svc.c */
int host_svc(unsigned num, jit_context *c);       /* 1 = answered */
extern int host_trace_svcs;
