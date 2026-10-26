/* Platform layer: the small set of OS services the Horizon host, the NVN layer and the CPU backend need.
   One implementation per OS (pal_win.c for Windows). Everything is C with fixed-width types: `long` is 32-bit on
   Windows, so it never appears in this interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- memory ---------------------------------------------------------------------------------------------------- */

enum { PAL_PROT_NONE = 0, PAL_PROT_READ = 1, PAL_PROT_WRITE = 2, PAL_PROT_EXEC = 4 };

size_t pal_page_size(void);           /* 4 KiB on Windows */
size_t pal_alloc_granularity(void);   /* 64 KiB on Windows: fixed views and reservations start on this boundary */

/* Reserve [addr, addr + size) without committing it, so nothing else in the process can take that range. The range
   can later be filled piece by piece with pal_map_view or pal_commit_private. Returns 0 on success. */
int pal_reserve_fixed(uintptr_t addr, size_t size);
/* The same, anywhere below limit (the system picks the address). Returns the address, 0 if there is no room. */
uintptr_t pal_reserve_below(size_t size, uintptr_t limit);
/* Release a whole reservation made with pal_reserve_fixed. Every view and private piece in it must be unmapped or
   released first. */
int pal_unreserve(uintptr_t addr, size_t size);
/* Reserve [addr, addr + size) for memory that grows and shrinks page by page (Horizon's heap, alias and stack
   regions): any page range in it can be committed with pal_commit and given back with pal_decommit, but it cannot
   hold section views. Committing pages that are already committed keeps their contents; recommitted pages read as
   zero. Released with pal_unreserve. Returns 0 on success. */
int pal_reserve_dynamic(uintptr_t addr, size_t size);

/* Anonymous memory backed by the page file that can be mapped at several addresses (Horizon's physical memory and
   its aliases). Pages are committed on first pal_commit of any view. */
typedef struct pal_section pal_section;
pal_section *pal_section_create(uint64_t size);
void pal_section_destroy(pal_section *s);
/* Map [offset, offset + size) of the section at addr, inside a range reserved with pal_reserve_fixed (addr == 0:
   anywhere). Returns the address, or 0 on failure. */
uintptr_t pal_map_view(pal_section *s, uint64_t offset, uintptr_t addr, size_t size, unsigned prot);
int pal_unmap_view(uintptr_t addr);

/* Private (not aliasable) memory inside a reserved range, e.g. guest stacks. */
int pal_commit_private(uintptr_t addr, size_t size, unsigned prot);
int pal_release_private(uintptr_t addr, size_t size);

/* Commit pages of a view (they become readable/writable with prot) and change protection. */
int pal_commit(uintptr_t addr, size_t size, unsigned prot);
int pal_protect(uintptr_t addr, size_t size, unsigned prot);
/* Tell the OS the contents are no longer needed (they may read back as anything, usually zero). */
int pal_discard(uintptr_t addr, size_t size);
/* Give committed pages back (they stay reserved; committing them again gives zero-filled pages). */
int pal_decommit(uintptr_t addr, size_t size);

/* Plain allocations for host use. */
void *pal_alloc(size_t size, unsigned prot);
void pal_free(void *p, size_t size);
void pal_flush_icache(const void *addr, size_t size);

/* ---- faults ---------------------------------------------------------------------------------------------------- */

enum { PAL_FAULT_READ = 0, PAL_FAULT_WRITE = 1, PAL_FAULT_EXEC = 2 };
/* Called on an access violation, on the faulting thread. Return 1 if handled (the access is retried), 0 to pass it
   on. Handlers run in registration order. Keep them short: on Windows they run on the faulting thread's stack. */
typedef int (*pal_fault_fn)(uintptr_t addr, int kind, void *os_context, void *user);
int pal_fault_add(pal_fault_fn fn, void *user);

/* Copy n bytes, returning 0, or 1 if a source or destination page faulted (nothing is reported as a crash). For
   reading guest memory the host does not own. */
int pal_guarded_copy(void *dst, const void *src, size_t n);

/* ---- threads and synchronization --------------------------------------------------------------------------- */

typedef struct pal_thread pal_thread;
typedef void (*pal_thread_fn)(void *arg);
pal_thread *pal_thread_create(pal_thread_fn fn, void *arg, size_t stack_size, const char *name);
void pal_thread_join(pal_thread *t);
void pal_thread_set_name(const char *name);
enum { PAL_PRIO_LOW = -1, PAL_PRIO_NORMAL = 0, PAL_PRIO_HIGH = 1, PAL_PRIO_REALTIME = 2 };
void pal_thread_set_priority(int prio);
uint64_t pal_thread_id(void);
void pal_yield(void);

typedef struct { void *opaque; } pal_mutex;    /* SRWLOCK; zero-initialized = unlocked */
#define PAL_MUTEX_INIT {0}
void pal_mutex_lock(pal_mutex *m);
int pal_mutex_trylock(pal_mutex *m);
void pal_mutex_unlock(pal_mutex *m);

typedef struct { void *opaque; } pal_cond;     /* CONDITION_VARIABLE; zero-initialized */
#define PAL_COND_INIT {0}
/* Returns 0 when woken, 1 on timeout. timeout_ns < 0 waits forever. */
int pal_cond_wait(pal_cond *c, pal_mutex *m, int64_t timeout_ns);
void pal_cond_signal(pal_cond *c);
void pal_cond_broadcast(pal_cond *c);

/* Futex-style wait: sleeps while *addr == expected. Returns 0 when woken (or the value differed), 1 on timeout. */
int pal_wait_u32(volatile uint32_t *addr, uint32_t expected, int64_t timeout_ns);
void pal_wake_one(volatile void *addr);
void pal_wake_all(volatile void *addr);

typedef uint32_t pal_tls_key;
pal_tls_key pal_tls_alloc(void);
void *pal_tls_get(pal_tls_key k);
void pal_tls_set(pal_tls_key k, void *v);

/* ---- time ------------------------------------------------------------------------------------------------------ */

uint64_t pal_now_ns(void);              /* monotonic */
uint64_t pal_wall_time_ns(void);        /* nanoseconds since 1970-01-01 UTC */
void pal_sleep_ns(uint64_t ns);         /* high-resolution where the OS offers it */

/* ---- files and paths (UTF-8 everywhere) -------------------------------------------------------------------- */

typedef struct pal_file pal_file;
enum { PAL_FILE_READ = 1, PAL_FILE_WRITE = 2, PAL_FILE_CREATE = 4, PAL_FILE_TRUNCATE = 8, PAL_FILE_EXCL = 16 };   /* EXCL: with CREATE, fail if it exists */
pal_file *pal_file_open(const char *path_utf8, unsigned mode);
void pal_file_close(pal_file *f);
int64_t pal_file_size(pal_file *f);
/* Positioned read/write; safe to call from several threads at once. Return bytes transferred, -1 on error. */
int64_t pal_file_pread(pal_file *f, void *buf, size_t n, uint64_t offset);
int64_t pal_file_pwrite(pal_file *f, const void *buf, size_t n, uint64_t offset);
int pal_file_flush(pal_file *f);
/* Read-only mapping of a whole file (the RomFS image). */
const void *pal_file_map(pal_file *f, uint64_t *size_out);
void pal_file_unmap(const void *p);

int pal_mkdirs(const char *path_utf8);           /* like mkdir -p; 0 on success or if it exists */

/* File and directory operations for the guest's file systems (save data). They return 0 or one of these: */
enum { PAL_ERR = -1, PAL_ENOENT = -2, PAL_EEXIST = -3, PAL_ENOTEMPTY = -4 };
int pal_last_file_error(void);                   /* why the last pal_file_open / pal_dir_open on this thread failed */
typedef struct { int is_dir; uint64_t size, mtime_ns, volume, index; } pal_stat;   /* volume + index: the file's identity */
int pal_stat_path(const char *path_utf8, pal_stat *out);
int pal_file_stat(pal_file *f, pal_stat *out);
int pal_file_truncate(pal_file *f, uint64_t size);
int pal_mkdir(const char *path_utf8);            /* one level; PAL_EEXIST if it is there */
int pal_remove_file(const char *path_utf8);
int pal_remove_dir(const char *path_utf8);       /* empty directories only (PAL_ENOTEMPTY) */
int pal_rename(const char *from, const char *to);   /* fails with PAL_EEXIST if the target exists */
typedef struct pal_dir pal_dir;
pal_dir *pal_dir_open(const char *path_utf8);
/* The next entry other than "." and "..": 1 with its name, 0 at the end. */
int pal_dir_next(pal_dir *d, char *name, size_t cap, int *is_dir, uint64_t *size);
void pal_dir_close(pal_dir *d);
int pal_rename_replace(const char *from, const char *to);   /* atomic replace on the same volume */
int pal_path_exists(const char *path_utf8);
/* %LOCALAPPDATA%\WinTears\<app> (created). Returns the length written, 0 on failure. */
size_t pal_user_data_dir(const char *app, char *out, size_t cap);
/* Everything written to stdout / stderr also goes to the file at path (still shown on the console); pal_tee_flush
   before a process ends without exit handlers (_Exit) so the last lines reach the file. 0 on success. */
int pal_tee_stdio(const char *path_utf8);
void pal_tee_flush(void);
size_t pal_exe_dir(char *out, size_t cap);

/* ---- misc ------------------------------------------------------------------------------------------------------ */

const char *pal_getenv(const char *name);       /* UTF-8; pointer valid until the next call on this thread */
void pal_debug_write(const char *text);
/* This process's memory: resident (working set) and private committed bytes. 0 on success. */
int pal_process_memory(uint64_t *resident, uint64_t *committed);
/* Where the process's memory is: committed and resident bytes per named range (the first that holds a region wins),
   then the rest by kind (private, mapped files, DLLs), and the largest unnamed private regions. One line into out. */
typedef struct { uintptr_t lo, hi; const char *name; } pal_range;
void pal_memory_breakdown(const pal_range *r, int n, char *out, size_t cap);
/* Running under Wine / Proton (the Steam Deck). */
int pal_under_wine(void);
/* How long a short sleep really takes here (twenty sleeps of `ns`; the average in ns), for the log. */
uint64_t pal_sleep_probe(uint64_t ns);
/* Access violations seen by the fault handler since the start (each is slow under Wine). */
uint64_t pal_fault_total(void);
/* The busiest threads since the last call: "name 87%, name 40%, ..." (percent of one core). */
void pal_thread_cpu_report(char *out, size_t cap);
/* Sampling profiler: a handle to the calling thread for pal_thread_sample. */
void *pal_thread_handle_self(void);
/* Stop thread h for a moment and describe where it is: "module+offset" of its pc (or "jit" for code outside any
   module), then up to two return addresses into this program found on its stack ("< exe+offset"). 0 if it failed. */
int pal_thread_sample(void *h, char *out, size_t cap);
/* Samples the other busy threads (2 % of a core or more since the last call), skipping the given thread ids and the
   caller: add(key) per sample, key = "name: where". */
void pal_sample_busy_threads(const unsigned long *skip, int nskip, void (*add)(const char *key, void *u), void *u);
unsigned long pal_thread_id_of(void *h);
/* Diagnostics for slow runs (mostly Wine / Proton on Linux; empty elsewhere): the environment once at start, the
   Linux view of this process and machine since the last call (scheduler, faults, frequencies, pressure, memory,
   cgroup throttling), and the exceptions seen. */
void pal_diag_boot(char *out, size_t cap);
void pal_linux_report(char *out, size_t cap);
void pal_exception_report(char *out, size_t cap);         /* stderr + debugger output */

#ifdef __cplusplus
}
#endif
