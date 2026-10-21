/* nn::hid for the functions main imports: one full-key controller (docked, player 1) or the handheld controller
   (TOTK_OPERATION_MODE=handheld). Ported from upstream games/totk/runtime/input.c and hid_feedback.c; the state ABI
   follows libnx HID.

   The pad state comes from the frontend (host_input_set_pad); until the SDL3 frontend exists nothing sets it, so the
   game sees a connected controller with nothing pressed. TOTK_INPUT_SCRIPT="sec:button:ms,..." presses buttons at
   given seconds after start (A, B, X, Y, L, R, ZL, ZR, PLUS, MINUS, UP, DOWN, LEFT, RIGHT, LSTICK, RSTICK): does the game
   react, without a picture.

   Rumble and the six-axis sensor (upstream hid_feedback.c) are off until the frontend has a controller to drive:
   the game then gets no vibration or six-axis handles at all, exactly as upstream with TOTK_RUMBLE=0 / TOTK_GYRO=0. */
#include <pthread.h>
#include <stdbool.h>

#include "host/hos.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 40, "Npad full-key ABI");
static pthread_mutex_t inputLock = PTHREAD_MUTEX_INITIALIZER;
static host_pad current;
static uint64_t pending;   /* presses since the last read, so a short press is never missed */
static uint32_t styles = 1;
static bool enabled, allowed = true;
static int holdType;
static int64_t sample;

void host_input_set_pad(const host_pad *p) {
    pthread_mutex_lock(&inputLock);
    pending |= p->buttons & ~current.buttons;
    current = *p;
    pthread_mutex_unlock(&inputLock);
}
void host_input_clear(void) {
    pthread_mutex_lock(&inputLock);
    memset(&current, 0, sizeof current); pending = 0;
    pthread_mutex_unlock(&inputLock);
}

static void initialize(void) {
    pthread_mutex_lock(&inputLock); enabled = true; pthread_mutex_unlock(&inputLock);
    LOG("controller initialized (no window yet: nothing pressed)");
}
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what, uint32_t id) {
    static int on = -1; static uint64_t seen[64]; static unsigned n;
    if (on < 0) on = pal_getenv("TOTK_INPUT_LOG") != NULL;
    if (!on) return;
    uint64_t k = (uint64_t)(uintptr_t)what << 32 ^ id ^ (uint64_t)styles << 48;
    for (unsigned i = 0; i < n; i++) if (seen[i] == k) return;
    if (n < 64) seen[n++] = k;
    LOG("[input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)", what, id, styles, allowed);
}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld: the same input is the handheld
   controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected; the game reads only the handheld
   controller in that mode. */
static uint32_t pad_id(void) { return runtime_handheld_mode() ? 0x20u : 0u; }
static uint32_t pad_style(void) { return runtime_handheld_mode() ? 2u : 1u; }
static void supported_ids(const uint32_t *ids, uint64_t n) {
    if (n > 10 || (!ids && n)) runtime_fail("invalid controller ids");
    pthread_mutex_lock(&inputLock);
    allowed = false;
    for (uint64_t i = 0; i < n; i++) if (ids[i] == pad_id()) allowed = true;
    pthread_mutex_unlock(&inputLock);
}
static void supported_styles(uint32_t value) { pthread_mutex_lock(&inputLock); styles = value; pthread_mutex_unlock(&inputLock); }
static uint32_t style(const uint32_t *id) {
    if (!id) runtime_fail("controller id pointer");
    input_log("style set", *id);
    pthread_mutex_lock(&inputLock);
    uint32_t v = enabled && allowed && *id == pad_id() ? (styles & pad_style()) : 0;
    pthread_mutex_unlock(&inputLock);
    return v;
}
static int pad_states(NpadState *out, int max, const uint32_t *id, uint32_t want_style) {
    if (max <= 0) return 0;
    if (id) input_log(want_style == 2 ? "handheld state" : "full-key state", *id);
    if (!out || !id) runtime_fail("controller output pointer");
    pthread_mutex_lock(&inputLock);
    NpadState state = {.sampling = ++sample};
    if (enabled && allowed && *id == pad_id() && want_style == pad_style() && (styles & want_style)) {
        state.attributes = 1;   /* connected */
        state.buttons = current.buttons | pending; pending = 0;
        state.lx = current.lx; state.ly = current.ly; state.rx = current.rx; state.ry = current.ry;
    }
    *out = state;
    pthread_mutex_unlock(&inputLock);
    return 1;
}
static int full_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 1); }
static int handheld_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 2); }
static int disconnected_states(NpadState *out, int max, const uint32_t *id) {
    if (id) input_log("joy-con state", *id);
    if (max <= 0) return 0;
    if (!out) runtime_fail("controller output pointer");
    pthread_mutex_lock(&inputLock); *out = (NpadState){.sampling = ++sample}; pthread_mutex_unlock(&inputLock);
    return 1;
}
static void hold_type(int type) { if (type < 0 || type > 1) runtime_fail("invalid joy hold type"); holdType = type; (void)holdType; }
static void disconnect(const uint32_t *id) {
    if (!id) runtime_fail("disconnect id");
    input_log("disconnect", *id);
    if (*id == pad_id()) {   /* only the pad in use: in handheld mode TOTK disconnects player 1 */
        pthread_mutex_lock(&inputLock); allowed = false; pthread_mutex_unlock(&inputLock);
        host_input_clear();
    }
}
/* Mouse and keyboard devices: the keyboard drives the controller, so the game sees no mouse (not connected) and a
   keyboard with no keys, avoiding duplicated input. Layouts follow the SDK (0x28 / 0x30 bytes). */
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "mouse/keyboard ABI");
static bool mouseReady, keyboardReady;
static void initialize_mouse(void) { pthread_mutex_lock(&inputLock); mouseReady = true; pthread_mutex_unlock(&inputLock); }
static void initialize_keyboard(void) { pthread_mutex_lock(&inputLock); keyboardReady = true; pthread_mutex_unlock(&inputLock); }
static int mouse_states(MouseState *out, int max) {
    if (max <= 0) return 0;
    if (!out) runtime_fail("mouse output pointer");
    pthread_mutex_lock(&inputLock);
    if (!mouseReady) runtime_fail("mouse used before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    pthread_mutex_unlock(&inputLock);
    return 1;
}
static void keyboard_state(KeyboardState *out) {
    if (!out) runtime_fail("keyboard output pointer");
    pthread_mutex_lock(&inputLock);
    if (!keyboardReady) runtime_fail("keyboard used before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    pthread_mutex_unlock(&inputLock);
}
/* Controller support applet (the "connect controllers" screen). TOTK opens it in handheld mode; there is exactly one
   controller here, so it returns at once: one player, on the controller in use (ControllerSupportResultInfo: int8
   player count, 3 bytes padding, uint32 selected id). */
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out, const void *arg) {
    (void)arg;
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    static int once; if (!once++) LOG("controller support applet: one player on controller %#x", pad_id());
    return 0;
}
/* No vibration devices or six-axis sensor yet (upstream hid_feedback.c with TOTK_RUMBLE=0 / TOTK_GYRO=0): the game asks
   for their handles only when a controller's style is set up, and gets none. */
static int no_handles(void *out, int max, const uint32_t *id, uint32_t style_set) { (void)out; (void)max; (void)id; (void)style_set; return 0; }

#define V(f) (void *)(f), HOST_RET_VOID
#define I32(f) (void *)(f), HOST_RET_U32
static const host_fn table[] = {
    {"_ZN2nn3hid14InitializeNpadEv", V(initialize)},
    {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", V(supported_ids)},
    {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", V(supported_styles)},
    {"_ZN2nn3hid15GetNpadStyleSetERKj", I32(style)},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", I32(full_states)},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", I32(disconnected_states)},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", I32(disconnected_states)},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", I32(disconnected_states)},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", I32(handheld_states)},
    {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", V(hold_type)},
    {"_ZN2nn3hid14DisconnectNpadERKj", V(disconnect)},
    {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", I32(show_controller_support)},
    {"_ZN2nn3hid15InitializeMouseEv", V(initialize_mouse)},
    {"_ZN2nn3hid18InitializeKeyboardEv", V(initialize_keyboard)},
    {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", I32(mouse_states)},
    {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", V(keyboard_state)},
    {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", I32(no_handles)},
    {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", I32(no_handles)},
};
const host_fn *host_input_override(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(name, table[i].name)) return &table[i];
    return NULL;
}

/* TOTK_INPUT_SCRIPT (upstream's takes Mac key codes; here button names) */
static uint64_t button_bit(const char *name, size_t n) {
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                        "LEFT", "UP", "RIGHT", "DOWN"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i]) == n && !strncmp(names[i], name, n)) return 1ull << i;
    return 0;
}
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = pal_now_ns();
    while (p && *p) {
        char *end; double at = strtod(p, &end); if (end == p || *end != ':') break;
        p = end + 1; size_t n = strcspn(p, ":"); uint64_t bit = button_bit(p, n); if (!bit || p[n] != ':') break;
        p += n + 1; unsigned long ms = strtoul(p, &end, 10); if (end == p) break;
        p = *end == ',' ? end + 1 : end;
        uint64_t when = t0 + (uint64_t)(at * 1e9);
        if (when > pal_now_ns()) pal_sleep_ns(when - pal_now_ns());
        LOG("[input] script: button %#llx down for %lu ms at %.1f s", (unsigned long long)bit, ms, at);
        host_pad pad = current; pad.buttons |= bit; host_input_set_pad(&pad);
        pal_sleep_ns((uint64_t)ms * 1000000);
        pad = current; pad.buttons &= ~bit; host_input_set_pad(&pad);
    }
    if (p && *p) LOG("[input] TOTK_INPUT_SCRIPT: stopped at \"%s\" (want sec:button:ms,...)", p);
    return NULL;
}
void host_input_start(void) {
    const char *e = pal_getenv("TOTK_INPUT_SCRIPT");
    if (!e || !*e) return;
    char *script = strdup(e);   /* pal_getenv's buffer belongs to this thread */
    pthread_t t;
    if (!script || pthread_create(&t, NULL, input_script, script)) die("input script thread");
    pthread_detach(t);
}
