#ifndef OPENPETE_WINDOW_RESTACK_H
#define OPENPETE_WINDOW_RESTACK_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Push the window to the bottom of the stacking order without activating, moving or resizing it.
 *
 * Only Win32 offers this: SetWindowPos with HWND_BOTTOM on the HWND taken
 * from the SDL window's properties. Elsewhere it does nothing and the
 * stacking order stays with the window manager. Called once at window
 * bring-up for --background, on the thread that owns the window.
 * @param win The SDL window. */
void openpete_window_send_to_back(SDL_Window *win);

/** @brief Estimate, before the window exists, the decorations a resizable SDL window adds around its client area on this platform.
 *
 * Win32 only: AdjustWindowRectExForDpi over the style SDL gives a resizable
 * window (WS_OVERLAPPED, WS_CAPTION, WS_SYSMENU, WS_MINIMIZEBOX,
 * WS_THICKFRAME, WS_MAXIMIZEBOX) at the display's DPI, the frame rectangle
 * GetWindowRect reports and SDL_GetWindowBordersSize measures once the
 * window exists. Without AdjustWindowRectExForDpi (before Windows 10 1607)
 * it uses AdjustWindowRectEx, the system DPI's frame. Elsewhere the size is
 * not known before the window maps and both results are 0.
 * @param scale The display's content scale (1.0 at 96 DPI); not positive reads as 1.0.
 * @param dw Receives the decorations' total width (left plus right).
 * @param dh Receives the decorations' total height (top plus bottom).
 * @return true when the estimate was computed (Win32), false when both results are 0. */
bool openpete_window_decoration_estimate(float scale, int *dw, int *dh);

#ifdef __cplusplus
}
#endif

#endif
