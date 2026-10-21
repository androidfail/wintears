/* Shared inside the Horizon host (not for the rest of the program). Plays the part of upstream's runtime.h and
   thread_compat.h for the ported files. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host.h"
#include "pal.h"

void host_log(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
_Noreturn void host_fail(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#define LOG(...) host_log(__VA_ARGS__)
#define die(...) host_fail(__VA_ARGS__)
_Noreturn void runtime_fail(const char *message);   /* upstream name */
#define now_ns pal_now_ns
#define guest_ns pal_now_ns
static inline int dm_detclock_enabled(void) { return 0; }

/* A host function replacing an SDK function, and how its result goes back to the guest's x0 (overrides.c). */
enum { HOST_RET_VOID, HOST_RET_U8, HOST_RET_U32, HOST_RET_U64,
       HOST_RET_VOID_FF };   /* two float arguments (v0, v1 -> xmm0, xmm1), no result */
typedef struct { const char *name; void *fn; int ret; } host_fn;

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
uint64_t host_object_page(void);                  /* a guest page for host tokens the game only passes back (vi Display, Layer) */
void *host_guest_alloc(size_t size);              /* zeroed guest-visible memory for host data the game reads; never freed */
void host_heap_range(uint64_t *base, uint64_t *size);   /* the guest heap now (SetHeapSize): one committed range */
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
void hle_bind_system_event(uint64_t ev, int autoclear);   /* guest SystemEventType: created by the SDK, signalled by the host */
void hle_signal_system_event(uint64_t ev);

/* upstream ipc.c (compiled unchanged) */
int runtime_ipc_svc(unsigned num, uint64_t *regs);
void runtime_trace_guest(uint64_t fp, uint64_t lr);
#include <pthread.h>
extern pthread_key_t runtime_keys[8];   /* [2]: the thread's TLR (IPC message buffer), as upstream */

/* sha256.c */
typedef struct { void *h; } host_sha256;
void host_sha256_begin(host_sha256 *s);
void host_sha256_update(host_sha256 *s, const void *p, size_t n);
void host_sha256_hex(host_sha256 *s, char hex[65]);

/* paths: the game data (<TOTK_win>/data) and the player's profile (saves, account) */
const char *host_data_dir(void);
const char *host_profile_dir(void);

/* romfs.c, fs_host.c, account.c (upstream files of the same names) */
void runtime_fs_init(void);
int runtime_romfs_owns(const char *path);
int runtime_romfs_handle(uint64_t h);
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode);
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_romfs_size(int64_t *out, uint64_t h);
void runtime_romfs_close(uint64_t h);
unsigned runtime_romfs_entry_type(int *out, const char *path);
void runtime_fs_log(const char *what, const char *path);
void runtime_account_uid(uint64_t out[2]);
const host_fn *host_fs_override(const char *name);        /* romfs.c + fs_host.c */
const host_fn *host_fs_host_override(const char *name);   /* fs_host.c */
const host_fn *host_account_override(const char *name);   /* account.c */
const host_fn *host_gfx_override(const char *name);       /* gfx_bridge.c */
uint64_t nvn_stub(const char *name);                       /* src/gfx/nvn.c: guest address for an NVN name, 0 if unknown */
const host_fn *host_vi_override(const char *name);        /* vi_stub.c */
const host_fn *host_input_override(const char *name);     /* input.c */
const host_fn *host_time_override(const char *name);      /* time.c */
const host_fn *host_font_override(const char *name);      /* fonts.c */
const host_fn *host_applet_override(const char *name);    /* applet.c */
const host_fn *host_codec_override(const char *name);     /* codec.c (upstream opus_codec.c) */
int runtime_handheld_mode(void);                           /* services.c: TOTK_OPERATION_MODE=handheld */
void runtime_oe_push_message(uint32_t message);            /* applet.c: an oe notification message for the game */

/* input.c: the controller state the frontend sets (libnx button bits: A 1, B 2, X 4, Y 8, L stick 16, R stick 32, L 64,
   R 128, ZL 256, ZR 512, Plus 1024, Minus 2048, Left 4096, Up 8192, Right 16384, Down 32768; sticks -32767..32767) */
typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } host_pad;
void host_input_set_pad(const host_pad *p);
void host_input_clear(void);
void host_input_start(void);                              /* TOTK_INPUT_SCRIPT */

/* overrides.c / services.c: host functions that replace SDK functions */
const host_fn *host_service_override(const char *name);   /* upstream runtime_override */
const host_fn *host_service_observer(const char *name);   /* upstream runtime_observer */

/* svc.c */
void dm_guest_write_prepare(void *addr, size_t n);                       /* upstream name: the host writes guest memory next */
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t n));     /* gfx/nvn_dirty.c */
int host_svc(unsigned num, jit_context *c);       /* 1 = answered */
extern int host_trace_svcs;
