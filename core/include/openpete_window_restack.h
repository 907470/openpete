#ifndef OPENPETE_WINDOW_RESTACK_H
#define OPENPETE_WINDOW_RESTACK_H

#include <SDL3/SDL.h>

/* Push the window to the bottom of the stacking order without activating,
 * moving or resizing it. Only Win32 offers this; elsewhere it does nothing
 * and the stacking order stays with the window manager. Called once, on the
 * thread that owns the window. */
void openpete_window_send_to_back(SDL_Window *win);

#endif
