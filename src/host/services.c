/* Host replacements for SDK functions, ported from upstream games/totk/runtime/services.c (runtime_override and
   runtime_observer, TOTK paths): the application environment (oe), add-on content, error reporting, parental
   controls and diagnostics start-up are answered locally; observers log the SDK's start-up steps and then run the
   game's own function. Other upstream override groups (fs, time, account, input, fonts, graphics...) join this table
   as they are ported. */
#include <windows.h>

#include "host/hos.h"

/* ---- simple start-up services ---------------------------------------------------------------------------------- */

static void host_diag_init(void) { LOG("native diagnostics initialized"); }
static void host_oe_init(void) { LOG("application environment: local Windows process"); }
static void host_aoc_init(void) { LOG("add-on content: no DLC"); }
static int host_aoc_count(void) { return 0; }
static int host_aoc_list(int *out, int offset, int count) {
    (void)out;
    if (offset < 0 || count < 0) die("invalid add-on content list range");
    return 0;
}
static void host_error_init(void) { LOG("error contexts: local diagnostics"); }
static void host_pctl_init(void) { LOG("local application parental controls initialized"); }
static void host_abort_init(void) { LOG("native crash observer initialized"); }
static void host_fs_init(void) { runtime_fs_init(); }
static void cpu_boost(int mode) { if (mode < 0 || mode > 2) die("unknown CPU boost mode"); LOG("CPU boost request %d: Windows manages CPU frequency", mode); }

/* TOTK_OPERATION_MODE=handheld reports handheld mode (oe OperationMode 0, PerformanceMode Normal 0) instead of docked
   (1, Boost 1). Read once; any other value stops the start. */
static int handheld_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = pal_getenv("TOTK_OPERATION_MODE");
        int h;
        if (!e || !strcmp(e, "docked")) h = 0;
        else if (!strcmp(e, "handheld")) h = 1;
        else die("TOTK_OPERATION_MODE must be docked or handheld");
        LOG("operation mode reported to the game: %s", h ? "handheld (performance normal)" : "docked (performance boost)");
        v = h;
    }
    return v;
}
int runtime_handheld_mode(void) { return handheld_mode(); }   /* input.c: the pad is the handheld controller then */
static int operation_mode(void) { return handheld_mode() ? 0 : 1; }
static int performance_mode(void) { return handheld_mode() ? 0 : 1; }
static int focus_policy;
static void focus_mode(int mode) {
    if (mode < 0 || mode > 3) die("unknown focus handling mode");
    focus_policy = mode;
    LOG("focus policy stored: %d (window lifecycle pending)", focus_policy);
}
static int focus_state(void) { return 1; }   /* InFocus: there is no window yet (upstream: runtime_front_focus) */
static unsigned resume_notification, operation_notification, performance_notification;
static void resume_enabled(unsigned value) { resume_notification = value & 255; }
static void operation_enabled(unsigned value) { operation_notification = value & 255; }
static void performance_enabled(unsigned value) { performance_notification = value & 255; }
static uint32_t performance_configuration[2];
static void performance_config(int mode, int cfg) {
    if (mode < 0 || mode > 1) die("invalid performance mode");
    performance_configuration[mode] = (uint32_t)cfg;
    LOG("performance request mode=%d cfg=%x: Windows scheduling", mode, (uint32_t)cfg);
}

/* The game's language: TOTK_LANG as given (a Switch language code), otherwise from TOTK_LOCALE or the Windows user
   locale (BCP 47, e.g. "pt-BR"), mapped as upstream maps the Mac's locale (1.0.0: Portuguese is "pt"). */
static uint64_t desired_language(void) {
    const char *explicit = pal_getenv("TOTK_LANG");
    char code[8] = {0};
    if (explicit) {
        if (strlen(explicit) > 7) die("language code too long");
        memcpy(code, explicit, strlen(explicit));
    } else {
        char tag[LOCALE_NAME_MAX_LENGTH * 3] = {0};
        const char *e = pal_getenv("TOTK_LOCALE");
        if (e && *e) snprintf(tag, sizeof tag, "%s", e);
        else {
            wchar_t w[LOCALE_NAME_MAX_LENGTH];
            if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH))
                WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, (int)sizeof tag, NULL, NULL);
        }
        char language[16] = {0};
        size_t n = strcspn(tag, "-_");
        if (n < sizeof language) memcpy(language, tag, n);
        static const char *known[] = {"ja", "de", "fr", "es", "it", "nl", "ru", "ko"};
        const char *selected = "en-US";
        for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (!strcmp(language, known[i])) selected = known[i];
        if (!strcmp(language, "zh")) selected = "zh-Hans";
        if (!strcmp(language, "pt")) selected = "pt";
        memcpy(code, selected, strlen(selected));
    }
    uint64_t result;
    memcpy(&result, code, 8);
    LOG("language: %s (TOTK_LANG=%s)", code, explicit ? explicit : "-");
    return result;
}

/* ---- observers: log, then run the game's own function ----------------------------------------------------------- */

static uint64_t call_original(const char *name, int n, const uint64_t *args) {
    uint64_t fn = host_symbol(name);
    if (!fn) die("observed function %s is missing", name);
    return host_guest_call(fn, n, args);
}
static void observed_os_init(uint64_t h, uint64_t args) {
    LOG("ENTER nnosInitialize");
    uint64_t a[2] = {h, args};
    call_original("nnosInitialize", 2, a);
    LOG("RETURN nnosInitialize");
}
static void observed_sdk_init(void) { LOG("ENTER nninitInitializeSdkModule"); call_original("nninitInitializeSdkModule", 0, NULL); LOG("RETURN nninitInitializeSdkModule"); }
static void observed_startup(void) { LOG("ENTER nninitStartup"); call_original("nninitStartup", 0, NULL); LOG("RETURN nninitStartup"); }
static void observed_main(void) { LOG("ENTER nnMain"); call_original("nnMain", 0, NULL); LOG("RETURN nnMain"); }
static uint32_t observed_region(uint64_t out, uint64_t size) {
    uint64_t a[2] = {out, size};
    uint32_t r = (uint32_t)call_original("_ZN2nn2os21AllocateAddressRegionEPmm", 2, a);
    if (r) LOG("AllocateAddressRegion size=%llx failed %#x", (unsigned long long)size, r);   /* one line per call was 4,700 per session */
    return r;
}
static uint32_t observed_pages(uint64_t addr, uint64_t size) {
    uint64_t a[2] = {addr, size};
    uint32_t r = (uint32_t)call_original("_ZN2nn2os19AllocateMemoryPagesEmm", 2, a);
    if (r) LOG("AllocateMemoryPages address=%llx size=%llx failed %#x", (unsigned long long)addr, (unsigned long long)size, r);   /* one line per call was 17,600 per session */
    return r;
}

#define V(f) (void *)(f), HOST_RET_VOID
#define I32(f) (void *)(f), HOST_RET_U32
#define I64(f) (void *)(f), HOST_RET_U64

static const host_fn observers[] = {
    {"_ZN2nn2os21AllocateAddressRegionEPmm", I32(observed_region)},
    {"_ZN2nn2os19AllocateMemoryPagesEmm", I32(observed_pages)},
    {"nnosInitialize", V(observed_os_init)},
    {"nninitInitializeSdkModule", V(observed_sdk_init)},
    {"nninitStartup", V(observed_startup)},
    {"nnMain", V(observed_main)},
};

static const host_fn overrides[] = {
    {"_ZN2nn2oe18GetDesiredLanguageEv", I64(desired_language)},
    {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", V(performance_config)},
    {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", V(cpu_boost)},
    {"_ZN2nn2oe16GetOperationModeEv", I32(operation_mode)},
    {"_ZN2nn2oe18GetPerformanceModeEv", I32(performance_mode)},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", V(focus_mode)},
    {"_ZN2nn2oe20GetCurrentFocusStateEv", I32(focus_state)},
    {"_ZN2nn2oe28SetResumeNotificationEnabledEb", V(resume_enabled)},
    {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", V(operation_enabled)},
    {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", V(performance_enabled)},
    {"nndiagStartup", V(host_diag_init)},
    {"_ZN2nn2oe10InitializeEv", V(host_oe_init)},
    {"_ZN2nn3aoc10InitializeEv", V(host_aoc_init)},
    {"_ZN2nn3aoc17CountAddOnContentEv", I32(host_aoc_count)},
    {"_ZN2nn3aoc16ListAddOnContentEPiii", I32(host_aoc_list)},
    {"_ZN2nn3err26InitializeContextWriterApiEv", V(host_error_init)},
    {"_ZN2nn2fs24InitializeForApplicationEv", V(host_fs_init)},
    {"_ZN2nn4pctl10InitializeEv", V(host_pctl_init)},
    {"_ZN2nn4diag34InitializeApplicationAbortObserverEv", V(host_abort_init)},
};

static const host_fn *find(const host_fn *t, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return &t[i];
    return NULL;
}
const host_fn *host_service_override(const char *name) {
    const host_fn *f = find(overrides, sizeof overrides / sizeof *overrides, name);
    if (!f) f = host_input_override(name);   /* upstream's order: input, fonts, codecs, movie, time, applet, swkbd, ... */
    if (!f) f = host_font_override(name);
    if (!f) f = host_codec_override(name);
    if (!f) f = host_movie_override(name);
    if (!f) f = host_time_override(name);
    if (!f) f = host_applet_override(name);
    if (!f) f = host_account_override(name);
    if (!f) f = host_fs_override(name);
    if (!f) f = host_gfx_override(name);
    if (!f) f = host_vi_override(name);
    return f;
}
const host_fn *host_service_observer(const char *name) { return find(observers, sizeof observers / sizeof *observers, name); }
