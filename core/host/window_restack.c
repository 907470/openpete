// core/host/window_restack.c
//
// The Win32 half of the background window: SDL has no portable call that
// lowers a window, so the HWND is taken from the SDL window's properties and
// placed at the bottom of the z-order.
#include "openpete_window_restack.h"

#ifdef _WIN32
#include <windows.h>
#endif

void openpete_window_send_to_back(SDL_Window *win)
{
#ifdef _WIN32
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                             NULL);
    if (hwnd)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
#else
    (void)win;
#endif
}
