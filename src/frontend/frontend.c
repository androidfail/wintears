/* SDL3 window and input. Keyboard keys follow upstream runtime/input.c (by position on a Mac keyboard: WASD left
   stick, arrows right stick, J A, K / Space B, I X, U Y, Q L, E R, Left Alt ZL, Left Shift ZR, Z left stick click,
   C right stick click, Enter Plus, Tab / Escape Minus, F G H T d-pad; Left Ctrl held: walk).
   Controllers are mapped by button position, the
   Switch's own layout (upstream controller_map.h default): east A, south B, north X, west Y; triggers are ZL / ZR.
   F11 toggles borderless fullscreen (not Alt+Enter: Alt is ZL). */
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
    case SDL_SCANCODE_J: return A;
    case SDL_SCANCODE_K: case SDL_SCANCODE_SPACE: return B;
    case SDL_SCANCODE_I: return X;
    case SDL_SCANCODE_U: return Y;
    case SDL_SCANCODE_Z: return LS;
    case SDL_SCANCODE_C: return RS;
    case SDL_SCANCODE_Q: return L;
    case SDL_SCANCODE_E: return R;
    case SDL_SCANCODE_LALT: return ZL;
    case SDL_SCANCODE_LSHIFT: return ZR;
    case SDL_SCANCODE_RETURN: return PLUS;
    case SDL_SCANCODE_TAB: case SDL_SCANCODE_ESCAPE: return MINUS;
    case SDL_SCANCODE_F: return LEFT;
    case SDL_SCANCODE_T: return UP;
    case SDL_SCANCODE_H: return RIGHT;
    case SDL_SCANCODE_G: return DOWN;
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

/* The pad state from everything held right now: keyboard and every open controller. */
static void update_pad(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    host_pad p = {0};
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) if (k[i]) p.buttons |= key_button((SDL_Scancode)i);
    int tilt = k[SDL_SCANCODE_LCTRL] ? 12000 : 32767;   /* Left Ctrl: the left stick part way (walk) */
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
        /* SDL's stick y grows downward, the Switch's upward */
        p.lx = larger(p.lx, axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)));
        p.ly = larger(p.ly, -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)));
        p.rx = larger(p.rx, axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX)));
        p.ry = larger(p.ry, -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY)));
    }
    host_input_set_pad(&p);
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
        if (g_pads[i]) LOG("[frontend] controller connected: %s", SDL_GetGamepadName(g_pads[i]));
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
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) continue;
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
