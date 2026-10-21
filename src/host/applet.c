/* Host side of the applet, telemetry, NFC and error-display functions main reaches. Ported from upstream
   games/totk/runtime/applet_host.c: client-side SDK code (PlayReport encoding, keyboard presets, diag logging) runs
   unchanged; only the calls that would talk to system services are taken here. */
#include <pthread.h>

#include "host/hos.h"

/* ---------- nn::oe ---------- */
static float balance[2] = {1.0f, 1.0f};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t message_cv = PTHREAD_COND_INITIALIZER;
static uint32_t messages[32]; static unsigned head, tail;
static uint64_t start_ns;
__attribute__((constructor)) static void started(void) { start_ns = pal_now_ns(); }

void runtime_oe_push_message(uint32_t message) {
    pthread_mutex_lock(&lock);
    if (tail - head < 32) { messages[tail++ % 32] = message; pthread_cond_broadcast(&message_cv); }
    pthread_mutex_unlock(&lock);
}
static void get_balance(float *l, float *r) { pthread_mutex_lock(&lock); if (l) *l = balance[0]; if (r) *r = balance[1]; pthread_mutex_unlock(&lock); }
static void set_balance(float l, float r) {
    if (l < 0 || l > 1 || r < 0 || r > 1) runtime_fail("volume balance out of range");
    pthread_mutex_lock(&lock); balance[0] = l; balance[1] = r; pthread_mutex_unlock(&lock);
}
static int64_t total_active_time(void) { return (int64_t)(pal_now_ns() - start_ns); }   /* TimeSpan in ns */
/* The host never sleeps the console; the extended inactivity window has no further effect. */
static void inactivity_extended(unsigned char on) { (void)on; }
/* No system startup logo is drawn on the host, so there is nothing to dismiss. */
static void finish_startup_logo(void) { LOG("game finished startup logo"); }
/* The single native executable was launched directly; it has no predecessor in an applet ExecuteProgram chain. -1
   keeps the game's normal user-open path. */
static int previous_program_index(void) { return -1; }
/* A stable, nonzero resource id for this process's one application. */
static uint64_t applet_resource_user_id(void) { return 1; }
static unsigned char try_pop_launch_parameter(uint64_t *size, void *buffer, uint64_t capacity) {
    (void)buffer; (void)capacity; if (size) *size = 0; return 0;
}
static unsigned char try_pop(uint32_t *out) {
    pthread_mutex_lock(&lock); int ok = head != tail; if (ok && out) *out = messages[head++ % 32]; pthread_mutex_unlock(&lock); return (unsigned char)ok;
}
static uint32_t pop(void) {
    __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();   /* blocks like a Horizon wait */
    pthread_mutex_lock(&lock); while (head == tail) pthread_cond_wait(&message_cv, &lock);
    uint32_t m = messages[head++ % 32]; pthread_mutex_unlock(&lock); return m;
}

/* ---------- nn::prepo: play reports are not transmitted (no Nintendo servers) ---------- */
static uint64_t reports;
static unsigned report_save(const void *report) {
    (void)report;
    if (!(reports++ % 100)) LOG("play report kept local (not transmitted), count %llu", (unsigned long long)reports);
    return 0;
}
static unsigned report_save_uid(const void *report, const void *uid) { (void)uid; return report_save(report); }
static unsigned immediate_transmission(void) { return 0; }
static unsigned connection_status(void *report, const char *a, const char *b, const char *c) { (void)report; (void)a; (void)b; (void)c; return 0; }

/* ---------- nn::nfp: the host has no NFC reader ---------- */
#define NFP_DEVICE_NOT_FOUND 0x8073u   /* nfp module 115, description 64 */
static int nfp_state;
static unsigned nfp_initialize(void) { nfp_state = 1; return 0; }
static unsigned nfp_finalize(void) { nfp_state = 0; return 0; }
static int nfp_get_state(void) { return nfp_state; }
static unsigned nfp_list(void *handles, int *count, int max) { (void)handles; (void)max; if (count) *count = 0; return NFP_DEVICE_NOT_FOUND; }
static unsigned nfp_attach_availability(uint64_t event) { if (!event) runtime_fail("nfp availability event"); hle_bind_system_event(event, 0); return 0; }
static unsigned nfp_no_device(void) { return NFP_DEVICE_NOT_FOUND; }
static int nfp_device_state(const void *h) { (void)h; return 0; }

/* ---------- nn::err ---------- */
static void show_error(uint32_t result) {
    LOG("game error dialog: %04u-%04u (result %#x)", 2000 + (result & 0x1ff), (result >> 9) & 0x1fff, result);
}

#define V(f) (void *)(f), HOST_RET_VOID
#define U8(f) (void *)(f), HOST_RET_U8
#define I32(f) (void *)(f), HOST_RET_U32
#define I64(f) (void *)(f), HOST_RET_U64
static const host_fn table[] = {
    {"_ZN2nn6applet23GetPreviousProgramIndexEv", I32(previous_program_index)},
    {"_ZN2nn6applet23GetAppletResourceUserIdEv", I64(applet_resource_user_id)},
    {"_ZN2nn2am23GetAppletResourceUserIdEv", I64(applet_resource_user_id)},
    {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", U8(try_pop_launch_parameter)},
    {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", V(get_balance)},
    {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", (void *)set_balance, HOST_RET_VOID_FF},
    {"_ZN2nn2oe25GetProgramTotalActiveTimeEv", I64(total_active_time)},
    {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", V(inactivity_extended)},
    {"_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", V(inactivity_extended)},
    {"_ZN2nn2oe17FinishStartupLogoEv", V(finish_startup_logo)},
    {"_ZN2nn2oe25TryPopNotificationMessageEPj", U8(try_pop)},
    {"_ZN2nn2oe22PopNotificationMessageEv", I32(pop)},
    {"_ZN2nn5prepo10PlayReport4SaveEv", I32(report_save)},
    {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", I32(report_save_uid)},
    {"_ZN2nn5prepo28RequestImmediateTransmissionEv", I32(immediate_transmission)},
    {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", I32(connection_status)},
    {"_ZN2nn3nfp10InitializeEv", I32(nfp_initialize)},
    {"_ZN2nn3nfp8FinalizeEv", I32(nfp_finalize)},
    {"_ZN2nn3nfp8GetStateEv", I32(nfp_get_state)},
    {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", I32(nfp_list)},
    {"_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", I32(nfp_attach_availability)},
    {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", I32(nfp_device_state)},
    {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", I32(nfp_no_device)},
    {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", I32(nfp_no_device)},
    {"_ZN2nn3err9ShowErrorENS_6ResultE", V(show_error)},
};
const host_fn *host_applet_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}
