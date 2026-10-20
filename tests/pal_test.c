/* Platform layer tests. No game data. Run on Windows or under Wine. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pal.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Horizon needs guest memory below 2^39. Use the alias-region address MNG uses on macOS 26. */
#define BASE 0x7000000000ull
#define RESERVE (8ull << 30)
#define GiB (1ull << 30)
#define MiB (1ull << 20)

#include <windows.h>
static int is_wine(void) { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL; }

static void test_reserve_and_alias(void) {
    CHECK(pal_page_size() == 4096);
    CHECK(pal_alloc_granularity() == 65536);
    CHECK(pal_reserve_fixed(BASE, RESERVE) == 0);
    pal_section *s = pal_section_create(64 * MiB);
    CHECK(s != NULL);
    uintptr_t a = pal_map_view(s, 0, BASE, 64 * MiB, PAL_PROT_READ | PAL_PROT_WRITE);
    uintptr_t b = pal_map_view(s, 0, BASE + GiB, 64 * MiB, PAL_PROT_READ | PAL_PROT_WRITE);
    CHECK(a == BASE);
    CHECK(b == BASE + GiB);
    /* a reserved range with nothing mapped faults, and the guarded copy reports it instead of crashing */
    uint32_t v = 0;
    CHECK(pal_guarded_copy(&v, (void *)(BASE + 5 * GiB), sizeof v) == 1);
    /* SEC_RESERVE: nothing is committed yet, so a read faults (Wine commits sections at once: skipped there) */
    if (!is_wine()) CHECK(pal_guarded_copy(&v, (void *)(a + 4096), sizeof v) == 1);
    CHECK(pal_commit(a, 1 * MiB, PAL_PROT_READ | PAL_PROT_WRITE) == 0);
    CHECK(pal_guarded_copy(&v, (void *)(a + 4096), sizeof v) == 0 && v == 0);
    ((volatile uint32_t *)a)[1024] = 0xC0FFEEu;
    CHECK(((volatile uint32_t *)b)[1024] == 0xC0FFEEu);   /* committed through view A, visible through view B */
    /* a view in the middle of a mapped range, at an offset into the section */
    uintptr_t c = pal_map_view(s, 0, BASE + 2 * GiB + 4 * MiB, 1 * MiB, PAL_PROT_READ);
    CHECK(c == BASE + 2 * GiB + 4 * MiB);
    CHECK(((volatile uint32_t *)c)[1024] == 0xC0FFEEu);
    /* unmap and map again at the same place: the placeholder is kept */
    CHECK(pal_unmap_view(b) == 0);
    CHECK(pal_map_view(s, 0, BASE + GiB, 64 * MiB, PAL_PROT_READ) == BASE + GiB);
    CHECK(((volatile uint32_t *)(BASE + GiB))[1024] == 0xC0FFEEu);
    /* private memory (a guest stack) inside the reservation, then given back and mapped as a view */
    uintptr_t st = BASE + 3 * GiB;
    CHECK(pal_commit_private(st, 256 * 1024, PAL_PROT_READ | PAL_PROT_WRITE) == 0);
    memset((void *)st, 0x5A, 256 * 1024);
    CHECK(((volatile uint8_t *)st)[1000] == 0x5A);
    CHECK(pal_release_private(st, 256 * 1024) == 0);
    CHECK(pal_map_view(s, 0, st, 256 * 1024, PAL_PROT_READ) == st);   /* a view fits the freed piece exactly */
    CHECK(pal_unmap_view(st) == 0);
    CHECK(pal_unmap_view(BASE + GiB) == 0);
    CHECK(pal_unmap_view(c) == 0);
    CHECK(pal_unmap_view(a) == 0);
    pal_section_destroy(s);
    CHECK(pal_unreserve(BASE, RESERVE) == 0);
    /* the whole range is free again */
    CHECK(pal_reserve_fixed(BASE, RESERVE) == 0);
    CHECK(pal_unreserve(BASE, RESERVE) == 0);
}

/* Dirty-page tracking in miniature: write-protect, catch the write, record it, unprotect, retry. */
static uintptr_t watch_lo, watch_hi;
static atomic_int write_faults;
static uintptr_t last_fault;
static int on_fault(uintptr_t addr, int kind, void *ctx, void *user) {
    (void)ctx;
    CHECK(user == (void *)&watch_lo);
    if (addr < watch_lo || addr >= watch_hi || kind != PAL_FAULT_WRITE) return 0;
    last_fault = addr;
    atomic_fetch_add(&write_faults, 1);
    uintptr_t page = addr & ~(uintptr_t)4095;
    return pal_protect(page, 4096, PAL_PROT_READ | PAL_PROT_WRITE) == 0;
}

static void test_write_tracking(void) {
    uint8_t *p = (uint8_t *)pal_alloc(16 * 4096, PAL_PROT_READ | PAL_PROT_WRITE);
    CHECK(p != NULL);
    watch_lo = (uintptr_t)p; watch_hi = watch_lo + 16 * 4096;
    CHECK(pal_fault_add(on_fault, &watch_lo) == 0);
    CHECK(pal_protect(watch_lo, 16 * 4096, PAL_PROT_READ) == 0);
    volatile uint8_t *q = p;
    CHECK(q[100] == 0);              /* reads do not fault */
    q[5 * 4096 + 7] = 1;             /* first write to page 5: one fault */
    q[5 * 4096 + 8] = 2;             /* page 5 is writable now: no fault */
    q[9 * 4096] = 3;                 /* page 9: second fault */
    CHECK(atomic_load(&write_faults) == 2);
    CHECK(last_fault == watch_lo + 9 * 4096);
    CHECK(q[5 * 4096 + 7] == 1 && q[5 * 4096 + 8] == 2 && q[9 * 4096] == 3);
    watch_hi = watch_lo;             /* stop watching */
    pal_free(p, 16 * 4096);
}

static volatile uint32_t flag;
static void waiter(void *arg) {
    (void)arg;
    while (flag == 0) pal_wait_u32(&flag, 0, -1);
}

static pal_mutex mu = PAL_MUTEX_INIT;
static pal_cond cv = PAL_COND_INIT;
static int counter, ready;
static void worker(void *arg) {
    (void)arg;
    pal_mutex_lock(&mu);
    while (!ready) pal_cond_wait(&cv, &mu, -1);
    pal_mutex_unlock(&mu);
    for (int i = 0; i < 10000; i++) { pal_mutex_lock(&mu); counter++; pal_mutex_unlock(&mu); }
}

static void test_threads(void) {
    pal_thread *w = pal_thread_create(waiter, NULL, 64 * 1024, "waiter");
    CHECK(w != NULL);
    pal_sleep_ns(5000000);
    flag = 1;
    pal_wake_all(&flag);
    pal_thread_join(w);
    uint32_t z = 0;
    uint64_t t0 = pal_now_ns();
    CHECK(pal_wait_u32(&z, 0, 3000000) == 1);   /* times out */
    CHECK(pal_now_ns() - t0 >= 2000000);
    CHECK(pal_wait_u32(&z, 1, 3000000) == 0);   /* value differs: returns at once */

    pal_thread *t[4];
    for (int i = 0; i < 4; i++) t[i] = pal_thread_create(worker, NULL, 0, "worker");
    pal_mutex_lock(&mu); ready = 1; pal_cond_broadcast(&cv); pal_mutex_unlock(&mu);
    for (int i = 0; i < 4; i++) pal_thread_join(t[i]);
    CHECK(counter == 40000);
    pal_mutex_lock(&mu);
    CHECK(pal_cond_wait(&cv, &mu, 2000000) == 1);
    pal_mutex_unlock(&mu);
    pal_tls_key k = pal_tls_alloc();
    pal_tls_set(k, &counter);
    CHECK(pal_tls_get(k) == &counter);
}

static void test_time(void) {
    uint64_t a = pal_now_ns();
    pal_sleep_ns(2000000);
    uint64_t b = pal_now_ns();
    CHECK(b > a && b - a >= 1500000);
    CHECK(pal_wall_time_ns() > 1700000000ull * 1000000000ull);   /* after 2023 */
}

static void test_files(void) {
    char dir[1024], path[1200], path2[1200];
    CHECK(pal_user_data_dir("pal-test", dir, sizeof dir) > 0);
    CHECK(pal_path_exists(dir));
    snprintf(path, sizeof path, "%s\\sub\\deeper", dir);
    CHECK(pal_mkdirs(path) == 0);
    snprintf(path, sizeof path, "%s\\sub\\deeper\\f\xC3\xA9.bin", dir);   /* non-ASCII name: UTF-8 path */
    snprintf(path2, sizeof path2, "%s\\sub\\deeper\\g.bin", dir);
    pal_file *f = pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE);
    CHECK(f != NULL);
    if (!f) return;
    char buf[8192];
    for (int i = 0; i < 8192; i++) buf[i] = (char)(i * 7);
    CHECK(pal_file_pwrite(f, buf, sizeof buf, 0) == 8192);
    CHECK(pal_file_pwrite(f, "tail", 4, 8192) == 4);
    CHECK(pal_file_size(f) == 8196);
    char r[16] = {0};
    CHECK(pal_file_pread(f, r, 4, 8192) == 4 && !memcmp(r, "tail", 4));
    CHECK(pal_file_pread(f, r, 16, 8190) == 6);   /* short read at the end */
    uint64_t sz = 0;
    const uint8_t *m = (const uint8_t *)pal_file_map(f, &sz);
    CHECK(m && sz == 8196 && m[1000] == (uint8_t)(1000 * 7));
    pal_file_unmap(m);
    pal_file_close(f);
    CHECK(pal_rename_replace(path, path2) == 0);
    CHECK(!pal_path_exists(path) && pal_path_exists(path2));
    char exe[1024];
    CHECK(pal_exe_dir(exe, sizeof exe) > 0);
}

int main(void) {
    test_reserve_and_alias();
    test_write_tracking();
    test_threads();
    test_time();
    test_files();
    printf("pal_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
