/* Offline local user identity. Ported from upstream games/totk/runtime/account.c: one local user whose id is created
   once and kept in <profile>/account.uid; handles are validated. */
#define _CRT_RAND_S   /* rand_s */
#include <stdlib.h>
#include <pthread.h>

#include "host/hos.h"

typedef struct { uint64_t lo, hi; } Uid;
typedef struct { Uid uid; uint64_t token; } UserHandle;
static Uid local;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tokens[64], next_token = 1;

static void initialize(void) {
    pthread_mutex_lock(&lock);
    if (local.lo || local.hi) { pthread_mutex_unlock(&lock); return; }
    char path[1200];
    snprintf(path, sizeof path, "%s/account.uid", host_profile_dir());
    pal_file *f = pal_file_open(path, PAL_FILE_READ | PAL_FILE_WRITE | PAL_FILE_CREATE);
    if (!f) runtime_fail("open local account identity");
    int64_t size = pal_file_size(f);
    if (size == 0) {
        do {
            unsigned w[4];
            for (int i = 0; i < 4; i++) rand_s(&w[i]);
            local.lo = (uint64_t)w[0] << 32 | w[1];
            local.hi = (uint64_t)w[2] << 32 | w[3];
        } while (!local.lo && !local.hi);
        if (pal_file_pwrite(f, &local, sizeof local, 0) != sizeof local || pal_file_flush(f)) runtime_fail("persist local account identity");
    } else if (size != sizeof local || pal_file_pread(f, &local, sizeof local, 0) != sizeof local || (!local.lo && !local.hi)) {
        runtime_fail("corrupt local account identity");
    }
    pal_file_close(f);
    pthread_mutex_unlock(&lock);
    LOG("persistent local account initialized");
}
static int same(const Uid *uid) { return uid && uid->lo == local.lo && uid->hi == local.hi; }
static int valid(const UserHandle *h) {
    if (!h || !same(&h->uid)) return 0;
    for (unsigned i = 0; i < 64; i++) if (tokens[i] && tokens[i] == h->token) return 1;
    return 0;
}
static unsigned preselected(UserHandle *out) {
    if (!out) runtime_fail("account null user handle");
    initialize();
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 64; i++)
        if (!tokens[i]) {
            tokens[i] = next_token++;
            *out = (UserHandle){local, tokens[i]};
            pthread_mutex_unlock(&lock);
            return 1;
        }
    pthread_mutex_unlock(&lock);
    return 0;
}
static uint32_t user_id(Uid *out, const UserHandle *h) {
    pthread_mutex_lock(&lock);
    int ok = out && valid(h);
    if (ok) *out = local;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : 0x7c67;
}
static void close_user(const UserHandle *h) {
    pthread_mutex_lock(&lock);
    if (!valid(h)) runtime_fail("invalid local account handle close");
    for (unsigned i = 0; i < 64; i++) if (tokens[i] == h->token) { tokens[i] = 0; break; }
    pthread_mutex_unlock(&lock);
}
static uint32_t existence(unsigned char *out, const Uid *uid) { if (!out || !uid) return 0x7c67; initialize(); *out = (unsigned char)same(uid); return 0; }
static uint32_t nickname(char *out, const Uid *uid) {
    initialize();
    if (!out || !same(uid)) return 0x7c67;
    memset(out, 0, 33);
    memcpy(out, "Local", 5);
    return 0;
}
void runtime_account_uid(uint64_t out[2]) { initialize(); out[0] = local.lo; out[1] = local.hi; }

static const host_fn entries[] = {
    {"_ZN2nn7account10InitializeEv", (void *)initialize, HOST_RET_VOID},
    {"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", (void *)preselected, HOST_RET_U8},
    {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE", (void *)close_user, HOST_RET_VOID},
    {"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", (void *)user_id, HOST_RET_U32},
    {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", (void *)existence, HOST_RET_U32},
    {"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", (void *)nickname, HOST_RET_U32},
};
const host_fn *host_account_override(const char *name) {
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return &entries[i];
    return NULL;
}
