/* SDL3 window and input. Keyboard and mouse as in a PC action game, for TOTK's controls: WASD move (left stick; Left Alt
   held: walk), arrows camera (right stick), Space jump (X), Left Shift / Backspace sprint and back (B), E / Enter
   interact and confirm (A), left click / F attack (Y), right click / V bow (ZR), Left Ctrl lock-on and shield (ZL),
   Q abilities (L), R throw / use (R), C crouch (left stick click), middle click / T right stick click, Esc menu (Plus),
   Tab / M map (Minus), 1 2 3 4 d-pad up, right, down, left.
   Controllers are mapped by button position, the
   Switch's own layout (upstream controller_map.h default): east A, south B, north X, west Y; triggers are ZL / ZR.
   F11 toggles borderless fullscreen (not Alt+Enter: Alt is walk). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "frontend/frontend.h"
#include "host/hos.h"

enum { A = 1, B = 2, X = 4, Y = 8, LS = 16, RS = 32, L = 64, R = 128, ZL = 256, ZR = 512, PLUS = 1024, MINUS = 2048,
       LEFT = 4096, UP = 8192, RIGHT = 16384, DOWN = 32768 };

static SDL_Window *g_win;
static SDL_Gamepad *g_pads[8];

static uint64_t key_button(SDL_Scancode k) {
    switch (k) {
    case SDL_SCANCODE_E: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return A;   /* interact, confirm */
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_BACKSPACE: return B;                       /* sprint, back */
    case SDL_SCANCODE_SPACE: return X;                                                      /* jump */
    case SDL_SCANCODE_F: return Y;                                                          /* attack */
    case SDL_SCANCODE_C: return LS;                                                         /* crouch */
    case SDL_SCANCODE_T: return RS;
    case SDL_SCANCODE_Q: return L;                                                          /* abilities */
    case SDL_SCANCODE_R: return R;                                                          /* throw / use */
    case SDL_SCANCODE_LCTRL: return ZL;                                                     /* lock-on, shield */
    case SDL_SCANCODE_V: return ZR;                                                         /* bow */
    case SDL_SCANCODE_ESCAPE: return PLUS;                                                  /* menu */
    case SDL_SCANCODE_TAB: case SDL_SCANCODE_M: return MINUS;                               /* map */
    case SDL_SCANCODE_1: return UP;
    case SDL_SCANCODE_2: return RIGHT;
    case SDL_SCANCODE_3: return DOWN;
    case SDL_SCANCODE_4: return LEFT;
    default: return 0;
    }
}

static const struct { SDL_GamepadButton b; uint64_t bit; } pad_map[] = {
    {SDL_GAMEPAD_BUTTON_EAST, A}, {SDL_GAMEPAD_BUTTON_SOUTH, B}, {SDL_GAMEPAD_BUTTON_NORTH, X}, {SDL_GAMEPAD_BUTTON_WEST, Y},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RS},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R},
    {SDL_GAMEPAD_BUTTON_START, PLUS}, {SDL_GAMEPAD_BUTTON_BACK, MINUS},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, UP}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN},
};

static int32_t axis(int v) { return v < -32767 ? -32767 : v; }               /* SDL's -32768 .. 32767 to the Switch's range */
static int32_t larger(int32_t a, int32_t b) { return abs(a) >= abs(b) ? a : b; }

/* A stick at rest is not at 0: the Steam Deck's read up to about 4800 (15%) and the game took it as a push, so Link
   drifted. Inside 16% of full tilt is the middle; the rest is stretched back to the full range (upstream: 12% per axis). */
static void stick(SDL_Gamepad *g, SDL_GamepadAxis ax, SDL_GamepadAxis ay, int32_t *x, int32_t *y) {
    float fx = axis(SDL_GetGamepadAxis(g, ax)) / 32767.f, fy = -axis(SDL_GetGamepadAxis(g, ay)) / 32767.f;   /* SDL's y grows downward */
    float m = SDL_sqrtf(fx * fx + fy * fy), dz = 0.16f;
    if (m <= dz) return;
    float s = ((m > 1 ? 1 : m) - dz) / (1 - dz) * 32767.f / m;   /* same direction, length stretched from dz..1 to 0..1 */
    *x = larger(*x, (int32_t)(fx * s)); *y = larger(*y, (int32_t)(fy * s));
}

/* The pad state from everything held right now: keyboard and every open controller. */
static void update_pad(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    host_pad p = {0};
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) if (k[i]) p.buttons |= key_button((SDL_Scancode)i);
    int tilt = k[SDL_SCANCODE_LALT] ? 12000 : 32767;   /* Left Alt: the left stick part way (walk) */
    {   /* mouse buttons (while the window has the mouse): left attack (Y), right bow (ZR), middle right stick click */
        SDL_MouseButtonFlags mb = SDL_GetMouseFocus() == g_win ? SDL_GetMouseState(NULL, NULL) : 0;
        if (mb & SDL_BUTTON_LMASK) p.buttons |= Y;
        if (mb & SDL_BUTTON_RMASK) p.buttons |= ZR;
        if (mb & SDL_BUTTON_MMASK) p.buttons |= RS;
    }
    p.lx = tilt * (k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]);
    p.ly = tilt * (k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]);
    p.rx = 32767 * (k[SDL_SCANCODE_RIGHT] - k[SDL_SCANCODE_LEFT]);
    p.ry = 32767 * (k[SDL_SCANCODE_UP] - k[SDL_SCANCODE_DOWN]);
    for (int i = 0; i < 8; i++) {
        SDL_Gamepad *g = g_pads[i];
        if (!g) continue;
        for (size_t j = 0; j < sizeof pad_map / sizeof *pad_map; j++) if (SDL_GetGamepadButton(g, pad_map[j].b)) p.buttons |= pad_map[j].bit;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) p.buttons |= ZL;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) p.buttons |= ZR;
        stick(g, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, &p.lx, &p.ly);
        stick(g, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, &p.rx, &p.ry);
    }
    host_input_set_pad(&p);
    /* the left stick's zone as the game gets it, when it changes (the first 1000: a stick that "stays held" on the
       Steam Deck shows here as "part way" with no return to the middle) */
    static int last_dir = -1, logged;
    int dir = abs(p.lx) < 4000 && abs(p.ly) < 4000 ? 0 : abs(p.lx) < 16000 && abs(p.ly) < 16000 ? 5
            : abs(p.lx) > abs(p.ly) ? (p.lx > 0 ? 1 : 2) : (p.ly > 0 ? 3 : 4);
    if (dir != last_dir && logged < 1000) {
        static const char *const names[] = {"middle", "right", "left", "up", "down", "part way"};
        LOG("[input] left stick %s (%d, %d) at %.3f s", names[dir], p.lx, p.ly, SDL_GetTicksNS() / 1e9); logged++;
    }
    last_dir = dir;
}

int fe_init(const char *title) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { LOG("[frontend] SDL_Init: %s", SDL_GetError()); return -1; }
    g_win = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) { LOG("[frontend] SDL_CreateWindow: %s", SDL_GetError()); SDL_Quit(); return -1; }
    LOG("[frontend] window 1280x720 (SDL %d.%d.%d)", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    return 0;
}

SDL_Window *fe_window(void) { return g_win; }
void fe_drawable_size(int *w, int *h) { if (!g_win || !SDL_GetWindowSizeInPixels(g_win, w, h)) *w = *h = 0; }

static void pad_added(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++) if (!g_pads[i]) {
        g_pads[i] = SDL_OpenGamepad(id);
        if (g_pads[i]) {   /* the path names SDL's input backend (XInput#n, a HID path for raw input / HIDAPI, ...) */
            const char *path = SDL_GetGamepadPath(g_pads[i]);
            LOG("[frontend] controller connected: %s (%s)", SDL_GetGamepadName(g_pads[i]), path ? path : "no path");
        }
        return;
    }
}
static void pad_removed(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++) if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
        LOG("[frontend] controller disconnected: %s", SDL_GetGamepadName(g_pads[i]));
        SDL_CloseGamepad(g_pads[i]); g_pads[i] = NULL;
    }
}

void fe_run(void) {
    for (;;) {
        /* every 8 ms at least, not only when an event comes: the controller's state is read again each time */
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 8)) { update_pad(); continue; }
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return;
            case SDL_EVENT_GAMEPAD_ADDED: pad_added(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_F11)
                    SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
                if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_F9) { void vk_capture_request(void); vk_capture_request(); }   /* log + save the next frame */
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: host_input_clear(); break;   /* no key stays held while another window has focus */
            default: break;
            }
        } while (SDL_PollEvent(&e));
        update_pad();
    }
}
