// core/host/window_restack.c
//
// Win32 window facts SDL has no portable call for: lowering a window (the
// background window; the HWND is taken from the SDL window's properties and
// placed at the bottom of the z-order), and the decoration size of a window
// before it is created.
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

bool openpete_window_decoration_estimate(float scale, int *dw, int *dh)
{
    *dw = *dh = 0;
#ifdef _WIN32
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                        WS_THICKFRAME | WS_MAXIMIZEBOX;
    const UINT dpi = (UINT)(96.0f * (scale > 0.0f ? scale : 1.0f) + 0.5f);
    RECT r = {0, 0, 0, 0};
    typedef BOOL(WINAPI * adjust_for_dpi_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const adjust_for_dpi_fn adjust_for_dpi =
        user32 ? (adjust_for_dpi_fn)(void *)GetProcAddress(user32, "AdjustWindowRectExForDpi") : NULL;
    const BOOL ok = adjust_for_dpi ? adjust_for_dpi(&r, style, FALSE, 0, dpi)
                                   : AdjustWindowRectEx(&r, style, FALSE, 0);
    if (!ok) return false;
    *dw = (int)(r.right - r.left);
    *dh = (int)(r.bottom - r.top);
    return true;
#else
    (void)scale;
    return false;
#endif
}
