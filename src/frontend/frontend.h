/* The window and the player's input (SDL3). The window lives on the process's main thread, which runs the event loop
   (fe_run); the game runs on another thread. Keyboard and controllers drive the one Switch controller (host/input.c). */
#pragma once
#include <stdint.h>

struct SDL_Window;

int fe_init(const char *title);          /* 0 = window created; -1 = no window (the reason is logged) */
void fe_run(void);                       /* event loop until the window closes; returns when it does */
struct SDL_Window *fe_window(void);      /* NULL without a window */
void fe_drawable_size(int *w, int *h);   /* the window's size in pixels */
