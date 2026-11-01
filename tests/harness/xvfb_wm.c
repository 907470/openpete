/* xvfb_wm.c - a minimal EWMH window manager for the harnesses' virtual X
 * displays (Xvfb has none, so a fullscreen or maximize request there does
 * nothing and the paths behind it never run).
 *
 * It maps every window where the client asks, applies every configure
 * request of a window it holds in the normal state, and answers the
 * WM_CHANGE_STATE iconify request with _NET_WM_STATE_HIDDEN and an unmap, and the
 * _NET_WM_STATE client message for _NET_WM_STATE_FULLSCREEN (the whole
 * screen) and _NET_WM_STATE_MAXIMIZED_VERT/HORZ (the work area): the
 * window's geometry before the change is kept and restored when the state is
 * removed, and the _NET_WM_STATE property is written as SDL reads it. The
 * root advertises _NET_SUPPORTING_WM_CHECK, _NET_SUPPORTED and _NET_WORKAREA,
 * the screen less a bottom panel of --panel N pixels (default 0), so SDL's
 * usable display bounds exclude it.
 *
 *   xvfb_wm [--panel N] [--ready FILE]   (run on $DISPLAY until the server exits)
 *
 * --ready FILE is created once the root advertises the manager, so a caller
 * starts its client only after a window manager is present.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_CLIENTS = 64 };

/* One managed window: its EWMH state and the geometry it returns to. */
typedef struct {
    Window win;
    int fullscreen, max_v, max_h;
    int iconic;                 /* unmapped by an iconify request until its next map */
    int saved;                  /* a normal-state geometry is kept */
    int sx, sy, sw, sh;         /* that geometry */
} client_t;

static Display *s_dpy;
static Window s_root;
static int s_sw, s_sh, s_panel;
static client_t s_clients[MAX_CLIENTS];
static Atom a_state, a_full, a_max_v, a_max_h, a_supported, a_check, a_name, a_utf8, a_workarea, a_change_state, a_hidden;

/** @brief Return the client record of a window, creating one when @p add is set.
 *
 * @param w The window.
 * @param add Non-zero to create a missing record.
 * @return The record, or NULL when absent (or the table is full). */
static client_t *client_of(Window w, int add)
{
    client_t *free_slot = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].win == w) return &s_clients[i];
        if (!s_clients[i].win && !free_slot) free_slot = &s_clients[i];
    }
    if (!add || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->win = w;
    return free_slot;
}

/** @brief Write a window's _NET_WM_STATE property from its record. */
static void write_state(const client_t *c)
{
    Atom v[4];
    int n = 0;
    if (c->iconic) v[n++] = a_hidden;
    if (c->fullscreen) v[n++] = a_full;
    if (c->max_v) v[n++] = a_max_v;
    if (c->max_h) v[n++] = a_max_h;
    XChangeProperty(s_dpy, c->win, a_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)v, n);
}

/** @brief Move and size a window to the geometry its state asks: the screen when fullscreen, the work area when maximized both ways, else the kept geometry. */
static void apply_state(client_t *c)
{
    const int special = c->fullscreen || (c->max_v && c->max_h);
    if (special && !c->saved) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(s_dpy, c->win, &wa)) {
            c->sx = wa.x; c->sy = wa.y; c->sw = wa.width; c->sh = wa.height;
            c->saved = 1;
            fprintf(stderr, "xvfb_wm: window %lx %s, returns to %dx%d\n", (unsigned long)c->win,
                    c->fullscreen ? "fullscreen" : "maximized", c->sw, c->sh);
        }
    }
    if (c->fullscreen) XMoveResizeWindow(s_dpy, c->win, 0, 0, (unsigned)s_sw, (unsigned)s_sh);
    else if (c->max_v && c->max_h) XMoveResizeWindow(s_dpy, c->win, 0, 0, (unsigned)s_sw, (unsigned)(s_sh - s_panel));
    else if (c->saved) {
        XMoveResizeWindow(s_dpy, c->win, c->sx, c->sy, (unsigned)c->sw, (unsigned)c->sh);
        c->saved = 0;
    }
    write_state(c);
}

/** @brief Set, clear or toggle one state flag per the _NET_WM_STATE action (0 remove, 1 add, 2 toggle). */
static void set_flag(int *flag, long action)
{
    *flag = action == 2 ? !*flag : action == 1;
}

/** @brief Act on a _NET_WM_STATE client message for the two properties it names. */
static void on_state_message(const XClientMessageEvent *e)
{
    client_t *c = client_of(e->window, 1);
    if (!c) return;
    for (int k = 1; k <= 2; k++) {
        const Atom p = (Atom)e->data.l[k];
        if (p == a_full) set_flag(&c->fullscreen, e->data.l[0]);
        else if (p == a_max_v) set_flag(&c->max_v, e->data.l[0]);
        else if (p == a_max_h) set_flag(&c->max_h, e->data.l[0]);
    }
    apply_state(c);
}

/** @brief Read the state a window asks for before it maps (an initial _NET_WM_STATE property) into its record. */
static void read_initial_state(client_t *c)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(s_dpy, c->win, a_state, 0, 16, False, XA_ATOM, &type, &fmt, &n, &after,
                           &data) != Success || !data)
        return;
    for (unsigned long i = 0; i < n; i++) {
        const Atom p = ((Atom *)data)[i];
        c->fullscreen |= p == a_full;
        c->max_v |= p == a_max_v;
        c->max_h |= p == a_max_h;
    }
    XFree(data);
}

/** @brief Apply a configure request: as asked for a window in the normal state; a fullscreen or maximized one keeps its geometry and stores the request as the geometry it returns to. */
static void on_configure(const XConfigureRequestEvent *e)
{
    client_t *c = client_of(e->window, 0);
    if (c && c->saved) {
        if (e->value_mask & CWWidth) c->sw = e->width;
        if (e->value_mask & CWHeight) c->sh = e->height;
        XWindowAttributes wa;
        if (XGetWindowAttributes(s_dpy, c->win, &wa)) {
            XConfigureEvent ce = {.type = ConfigureNotify, .display = s_dpy, .event = c->win,
                                  .window = c->win, .x = wa.x, .y = wa.y, .width = wa.width,
                                  .height = wa.height};
            XSendEvent(s_dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
        }
        return;
    }
    XWindowChanges wc = {.x = e->x, .y = e->y, .width = e->width, .height = e->height,
                         .border_width = e->border_width, .sibling = e->above, .stack_mode = e->detail};
    XConfigureWindow(s_dpy, e->window, (unsigned)e->value_mask, &wc);
}

/** @brief Advertise the window manager on the root: the check window, the supported atoms and the work area. */
static void advertise(void)
{
    Window check = XCreateSimpleWindow(s_dpy, s_root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(s_dpy, s_root, a_check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(s_dpy, check, a_check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(s_dpy, check, a_name, a_utf8, 8, PropModeReplace, (const unsigned char *)"xvfb_wm", 7);
    Atom sup[] = {a_state, a_full, a_max_v, a_max_h, a_hidden, a_check, a_workarea};
    XChangeProperty(s_dpy, s_root, a_supported, XA_ATOM, 32, PropModeReplace, (unsigned char *)sup,
                    (int)(sizeof sup / sizeof sup[0]));
    long wa[4] = {0, 0, s_sw, s_sh - s_panel};
    XChangeProperty(s_dpy, s_root, a_workarea, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)wa, 4);
}

/** @brief Act on one event the root's substructure redirect delivers: map, configure, state change, iconify, destroy.
 *
 * @param e The event. */
static void handle(XEvent *e)
{
    if (e->type == MapRequest) {
        client_t *c = client_of(e->xmaprequest.window, 1);
        XMapWindow(s_dpy, e->xmaprequest.window);
        if (!c) return;
        if (c->iconic) {
            c->iconic = 0;
            write_state(c);
        }
        read_initial_state(c);
        if (c->fullscreen || c->max_v || c->max_h) apply_state(c);
    } else if (e->type == ConfigureRequest) {
        on_configure(&e->xconfigurerequest);
    } else if (e->type == ClientMessage && e->xclient.message_type == a_state) {
        on_state_message(&e->xclient);
    } else if (e->type == ClientMessage && e->xclient.message_type == a_change_state &&
               e->xclient.data.l[0] == IconicState) {
        /* An iconify request (XIconifyWindow): _NET_WM_STATE_HIDDEN and unmap; the next MapRequest maps it
         * again and clears the state. */
        client_t *c = client_of(e->xclient.window, 1);
        if (c) {
            c->iconic = 1;
            write_state(c);
        }
        XUnmapWindow(s_dpy, e->xclient.window);
        fprintf(stderr, "xvfb_wm: window %lx iconified\n", (unsigned long)e->xclient.window);
    } else if (e->type == DestroyNotify) {
        client_t *c = client_of(e->xdestroywindow.window, 0);
        if (c) c->win = 0;
    }
}

/** @brief Intern the atoms the manager reads and writes. */
static void intern_atoms(void)
{
    a_state = XInternAtom(s_dpy, "_NET_WM_STATE", False);
    a_full = XInternAtom(s_dpy, "_NET_WM_STATE_FULLSCREEN", False);
    a_max_v = XInternAtom(s_dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    a_max_h = XInternAtom(s_dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    a_supported = XInternAtom(s_dpy, "_NET_SUPPORTED", False);
    a_check = XInternAtom(s_dpy, "_NET_SUPPORTING_WM_CHECK", False);
    a_name = XInternAtom(s_dpy, "_NET_WM_NAME", False);
    a_utf8 = XInternAtom(s_dpy, "UTF8_STRING", False);
    a_workarea = XInternAtom(s_dpy, "_NET_WORKAREA", False);
    a_change_state = XInternAtom(s_dpy, "WM_CHANGE_STATE", False);
    a_hidden = XInternAtom(s_dpy, "_NET_WM_STATE_HIDDEN", False);
}

/** @brief Run the window manager on $DISPLAY until the server goes away.
 *
 * @param argc Argument count.
 * @param argv --panel N reserves N pixels at the bottom of the work area;
 *             --ready FILE is created once the manager is advertised.
 * @return 0 when the server exits, 1 when the display cannot be opened. */
int main(int argc, char **argv)
{
    const char *ready = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--panel")) s_panel = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--ready")) ready = argv[i + 1];
    }
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy) { fprintf(stderr, "xvfb_wm: cannot open the display\n"); return 1; }
    s_root = DefaultRootWindow(s_dpy);
    s_sw = DisplayWidth(s_dpy, DefaultScreen(s_dpy));
    s_sh = DisplayHeight(s_dpy, DefaultScreen(s_dpy));
    intern_atoms();
    XSelectInput(s_dpy, s_root, SubstructureRedirectMask | SubstructureNotifyMask);
    advertise();
    XSync(s_dpy, False);
    fprintf(stderr, "xvfb_wm: managing %dx%d, panel %d\n", s_sw, s_sh, s_panel);
    if (ready) { FILE *f = fopen(ready, "w"); if (f) fclose(f); }
    for (;;) {
        XEvent e;
        XNextEvent(s_dpy, &e);
        handle(&e);
        XFlush(s_dpy);
    }
}
