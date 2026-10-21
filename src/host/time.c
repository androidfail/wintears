/* nn::time for the functions main imports, backed by the host clock and time zone. Ported from upstream
   games/totk/runtime/time_host.c. User and network system clocks both follow the (synchronised) Windows wall clock;
   the steady clock counts seconds since boot, sleep included (upstream: mach_continuous_time), with one source id per
   process. ClockSnapshot uses the public 0xD0-byte layout because game code may read its fields through inline
   accessors. The location name is the IANA name of the Windows time zone (ICU, part of Windows 10 1903 and later),
   "UTC" when it cannot be found. */
#define _CRT_RAND_S   /* rand_s */
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <windows.h>

#include "host/hos.h"

typedef struct { int64_t value; uint8_t source[16]; } SteadyClockTimePoint;
typedef struct { int64_t offset; SteadyClockTimePoint point; } SystemClockContext;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalendarAdditionalInfo;
typedef struct {
    SystemClockContext user, network; int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar; CalendarAdditionalInfo user_info, network_info;
    SteadyClockTimePoint steady; char location[0x24]; uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;
_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

#define R_INVALID 0x74u          /* time module 116: invalid argument family */
#define R_SOURCE_MISMATCH 0x18074u
static uint8_t source_id[16];
static int initialized;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void make_source(void) { for (int i = 0; i < 4; i++) { unsigned w; rand_s(&w); memcpy(source_id + 4 * i, &w, 4); } }

static int64_t steady_seconds(void) { return (int64_t)(GetTickCount64() / 1000); }
static SteadyClockTimePoint steady_now(void) {
    pthread_once(&once, make_source);
    SteadyClockTimePoint p = {steady_seconds(), {0}}; memcpy(p.source, source_id, 16); return p;
}
static int64_t posix_now(void) {
    static long long off = -1;
    if (off < 0) { const char *e = pal_getenv("TOTK_TIME_OFFSET"); off = e ? atoll(e) : 0; if (off < 0) off = 0; }   /* diagnostic: shift the clock the game sees (seconds) */
    return (int64_t)_time64(NULL) + off;
}

static unsigned initialize(void) { initialized = 1; pthread_once(&once, make_source); return 0; }
static unsigned finalize(void) { initialized = 0; return 0; }
static unsigned char is_initialized(void) { return (unsigned char)initialized; }
static unsigned user_time(int64_t *out) { if (!out) return R_INVALID; *out = posix_now(); return 0; }
/* StandardSteadyClock::now() returns the chrono-style time_point: the int64 seconds value only (sdk+0x3d1d88 returns
   it in x0). */
static int64_t steady_clock_now(void) { return steady_now().value; }

static void fill_calendar(CalendarTime *c, CalendarAdditionalInfo *info, int64_t posix, int local) {
    __time64_t t = (__time64_t)posix; struct tm tm;
    if (local ? _localtime64_s(&tm, &t) : _gmtime64_s(&tm, &t)) runtime_fail("calendar conversion");
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday; info->day_of_year = tm.tm_yday;
        int32_t offset = 0;
        if (local) { struct tm copy = tm; offset = (int32_t)(_mkgmtime64(&copy) - t); }
        /* Windows has no zone abbreviations: the offset in the form IANA uses for unnamed zones (+0530, -05) */
        unsigned a = (unsigned)(offset < 0 ? -offset : offset) % 86400, h = a / 3600, m = a % 3600 / 60;
        if (!local) snprintf(info->name, sizeof info->name, "UTC");
        else if (m) snprintf(info->name, sizeof info->name, "%c%02u%02u", offset < 0 ? '-' : '+', h, m);
        else snprintf(info->name, sizeof info->name, "%c%02u", offset < 0 ? '-' : '+', h);
        info->dst = local && tm.tm_isdst > 0; info->utc_offset = offset;
    }
}
static unsigned to_calendar(CalendarTime *c, CalendarAdditionalInfo *info, const int64_t *posix) {
    if (!c || !posix) return R_INVALID;
    fill_calendar(c, info, *posix, 1);
    return 0;
}
static CalendarTime to_calendar_utc(const int64_t *posix) { CalendarTime c; fill_calendar(&c, NULL, posix ? *posix : 0, 0); return c; }
static int64_t to_posix_utc(const CalendarTime *c) {
    struct tm tm = {0}; tm.tm_year = c->year - 1900; tm.tm_mon = c->month - 1; tm.tm_mday = c->day; tm.tm_hour = c->hour; tm.tm_min = c->minute; tm.tm_sec = c->second;
    return (int64_t)_mkgmtime64(&tm);
}

/* ICU's ucal_getTimeZoneIDForWindowsID (UChar is 16-bit, as wchar_t on Windows; status > 0 is an error) */
typedef int32_t (*windows_to_iana)(const wchar_t *winid, int32_t len, const char *region, wchar_t *id, int32_t cap, int *status);
static char zone[0x24];
static pthread_once_t zone_once = PTHREAD_ONCE_INIT;
static void find_zone(void) {
    memset(zone, 0, sizeof zone);
    snprintf(zone, sizeof zone, "UTC");
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return;
    HMODULE icu = LoadLibraryW(L"icu.dll");
    windows_to_iana f = icu ? (windows_to_iana)(void *)GetProcAddress(icu, "ucal_getTimeZoneIDForWindowsID") : NULL;
    wchar_t id[64]; int status = 0;
    int32_t n = f ? f(tz.TimeZoneKeyName, -1, NULL, id, 63, &status) : 0;
    char u[0x24];
    if (n > 0 && n < 64 && status <= 0) {
        id[n] = 0;
        if (WideCharToMultiByte(CP_UTF8, 0, id, -1, u, (int)sizeof u, NULL, NULL) > 0) snprintf(zone, sizeof zone, "%s", u);
    }
    LOG("time zone: %s", zone);
}
static void location_name(char out[0x24]) { pthread_once(&zone_once, find_zone); memcpy(out, zone, 0x24); }

static void snapshot_init(ClockSnapshot *s) { memset(s, 0, sizeof *s); }
static unsigned snapshot_create(ClockSnapshot *s) {
    if (!s) return R_INVALID;
    SteadyClockTimePoint steady = steady_now(); int64_t now = posix_now();
    memset(s, 0, sizeof *s);
    s->user = (SystemClockContext){now - steady.value, steady}; s->network = s->user;
    s->user_time = now; s->network_time = now;
    fill_calendar(&s->user_calendar, &s->user_info, now, 1); fill_calendar(&s->network_calendar, &s->network_info, now, 1);
    s->steady = steady; location_name(s->location); s->automatic_correction = 1; s->type = 0;
    return 0;
}
static unsigned span_between(int64_t *out_ns, const ClockSnapshot *a, const ClockSnapshot *b) {
    if (!out_ns || !a || !b) return R_INVALID;
    if (memcmp(a->steady.source, b->steady.source, 16)) return R_SOURCE_MISMATCH;
    *out_ns = (b->steady.value - a->steady.value) * 1000000000LL; return 0;
}
/* Change of the user clock offset between snapshots, i.e. manual adjustments, as a TimeSpan. */
static int64_t user_difference(const ClockSnapshot *a, const ClockSnapshot *b) {
    if (memcmp(a->user.point.source, b->user.point.source, 16)) return 0;
    return (b->user.offset - a->user.offset) * 1000000000LL;
}

#define V(f) (void *)(f), HOST_RET_VOID
#define U8(f) (void *)(f), HOST_RET_U8
#define I32(f) (void *)(f), HOST_RET_U32
#define I64(f) (void *)(f), HOST_RET_U64
static const host_fn table[] = {
    {"_ZN2nn4time10InitializeEv", I32(initialize)}, {"_ZN2nn4time8FinalizeEv", I32(finalize)}, {"_ZN2nn4time13IsInitializedEv", U8(is_initialized)},
    {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", I32(user_time)},
    {"_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", I32(user_time)},
    {"_ZN2nn4time19StandardSteadyClock3nowEv", I64(steady_clock_now)},
    {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", I32(to_calendar)},
    {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", I64(to_calendar_utc)},   /* an 8-byte struct: x0 on both sides */
    {"_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", I64(to_posix_utc)},
    {"_ZN2nn4time13ClockSnapshotC1Ev", V(snapshot_init)},
    {"_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", I32(snapshot_create)},
    {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", I32(span_between)},
    {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", I64(user_difference)},
};
const host_fn *host_time_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}
