#ifndef OPENPETE_WINDOW_RESTACK_H
#define OPENPETE_WINDOW_RESTACK_H

#include <SDL3/SDL.h>

/** @brief Push the window to the bottom of the stacking order without activating, moving or resizing it.
 *
 * Only Win32 offers this: SetWindowPos with HWND_BOTTOM on the HWND taken
 * from the SDL window's properties. Elsewhere it does nothing and the
 * stacking order stays with the window manager. Called once at window
 * bring-up for --background, on the thread that owns the window.
 * @param win The SDL window. */
void openpete_window_send_to_back(SDL_Window *win);

#endif
