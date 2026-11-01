/* launcher_internal.h - the calls between the launcher's screens
 * (launcher.cpp) and its disc library (library.cpp). */
#ifndef OPENPETE_LAUNCHER_INTERNAL_H
#define OPENPETE_LAUNCHER_INTERNAL_H

#include <stddef.h>

#include "openpete_launcher.h"

/* library_resolve results besides 0. */
#define LAUNCHER_LIB_FAILED (-1)   /* ingest failure or an unreadable entry */
#define LAUNCHER_LIB_QUIT   (-2)   /* the player quit the version selector */
#define LAUNCHER_LIB_BUILD_FAILED (-3)   /* without screens: the picked entry's module build failed */

/* launcher_select results besides an index. */
#define LAUNCHER_SEL_QUIT     (-1)  /* the player quit */
#define LAUNCHER_SEL_DROPPED  (-2)  /* a disc image was dropped instead */
#define LAUNCHER_SEL_NOSCREEN (-3)  /* no screen can show: take the most recent */

/* The launcher's start-up events, each logged once by launcher_stamp. */
enum launcher_event {
    LAUNCHER_EV_WINDOW,          /* the launcher's window and device exist */
    LAUNCHER_EV_FIRST_PRESENT,   /* the first launcher frame was presented */
    LAUNCHER_EV_SCAN_START,      /* the library scan began */
    LAUNCHER_EV_SCAN_END,        /* the library scan returned */
    LAUNCHER_EV_LIST_SHOWN,      /* the first selector frame with its entries was presented */
    LAUNCHER_EV_COUNT
};

/** @brief Log a start-up event once, in ms after process start.
 *
 * Process start is the launcher translation unit's static initialization,
 * before main. A later call for the same event is ignored. Safe on any
 * thread.
 * @param ev The event. */
void launcher_stamp(launcher_event ev);

/** @brief Report whether the launcher names no game: an executable linking several games, before one is installed.
 *
 * @return true while the descriptor passed to openpete_launcher_begin
 *         carries no game id. */
bool launcher_neutral(void);

/** @brief Take the harness drop OPENPETE_LAUNCHER_DROP once, as if that path had been dropped.
 *
 * The first call that finds the variable set stores its path for
 * launcher_dropped_path and returns it; every later call returns NULL, so
 * the scripted drop is ingested once per process.
 * @return The path, owned by the launcher, or NULL. */
const char* launcher_scripted_drop(void);

/** @brief The game the launcher was started for (openpete_launcher_begin).
 *
 * @return The game, or NULL before openpete_launcher_begin. */
const openpete_launcher_game* launcher_game(void);

/** @brief Run the game page over the library's entries (launcher_page_draw): each game's box picks its most recently played entry.
 *
 * Opens the launcher's window when it is not open. Left and right (keys,
 * d-pad, left stick) move the focus one box without wrapping; up and down
 * do nothing; the pointer takes the focus only after it moves. The focus
 * starts on the box of the game launcher_focus_game names, else the first.
 * Enter, the gamepad's south button or a click activates a box: a box with
 * an entry picks it, an empty box opens the file dialog. Escape or closing
 * the window quits at once; the east button asks once ("Press again to
 * quit") and a second press within LAUNCHER_QUIT_ASK_MS quits; a file
 * dropped on the window, or chosen in the file dialog Space or the
 * gamepad's west button opens, returns LAUNCHER_SEL_DROPPED with its path in
 * launcher_dropped_path. Entries that own no box are logged. The harness
 * variable OPENPETE_LAUNCHER_DROP acts as a drop after the first frame, and
 * without a screen at once (launcher_scripted_drop);
 * OPENPETE_LAUNCHER_SELECT set to an index below @p count picks that entry
 * after the first frame, and OPENPETE_LAUNCHER_SELECT_GAME set to a game id
 * picks the first entry of that game; with OPENPETE_LAUNCHER_SELECT_IDLE set
 * the scripted pick also waits until no launcher job runs or is queued.
 * Launcher jobs are pumped between frames (launcher_jobs_pump).
 * @param labels The entry labels, for the log.
 * @param games Each entry's game id ("" when unidentified), most recently played first.
 * @param count Their number, at least one.
 * @return The picked index, or a LAUNCHER_SEL_* value. */
int launcher_select(const char* const* labels, const char* const* games, int count);

/* The game page's focus over its boxes. Keys and the pad move it one box
 * (launcher_focus_move) and hold it there; the box under the pointer takes
 * it (launcher_focus_hover) only once the pointer has moved since
 * (launcher_focus_pointer_moved). */
struct launcher_focus {
    int index;   /* the focused box */
    int count;   /* the boxes */
    bool held;   /* a key or pad placed the focus and the pointer has not moved since */
};

/** @brief Move the focus one box left or right, without wrapping, and hold it against a still pointer.
 *
 * Logs the move, an edge press included.
 * @param f The focus.
 * @param dir Negative for left, positive for right.
 * @return 1 when the focused box changed, else 0. */
int launcher_focus_move(launcher_focus* f, int dir);

/** @brief Release a held focus: the pointer moved, so the box under it may take the focus.
 *
 * @param f The focus. */
void launcher_focus_pointer_moved(launcher_focus* f);

/** @brief Give the focus to the box under the pointer unless a key or pad holds it.
 *
 * @param f The focus.
 * @param hovered The box under the pointer, or -1 for none.
 * @return true when the focus moved to @p hovered. */
bool launcher_focus_hover(launcher_focus* f, int hovered);

/* How long, in ms, a first press of the east button waits for the second
 * that quits. */
#define LAUNCHER_QUIT_ASK_MS 2000

/* The east button's ask-once quit: the first press asks, a second within
 * LAUNCHER_QUIT_ASK_MS quits. Zero-initialised: nothing asked. */
struct launcher_quit_ask {
    unsigned long long asked_ms;   /* when the ask was made, 0 for none */
};

/** @brief Take one press of the east button: the first press asks, a second while the ask shows quits.
 *
 * @param q The ask state.
 * @param now_ms The press's time in ms on a monotonic clock (SDL_GetTicks).
 * @return true when this press quits. */
bool launcher_quit_ask_press(launcher_quit_ask* q, unsigned long long now_ms);

/** @brief Report whether the ask shows: a first press was made less than LAUNCHER_QUIT_ASK_MS ago.
 *
 * @param q The ask state.
 * @param now_ms The time in ms on the clock launcher_quit_ask_press was given.
 * @return true while the "Press again to quit" chip draws. */
bool launcher_quit_ask_shown(const launcher_quit_ask* q, unsigned long long now_ms);

/** @brief Name the game whose box takes the focus when the game page next shows: the game of a disc just added.
 *
 * @param game The game id; NULL or "" for none. */
void launcher_focus_game_set(const char* game);

/** @brief Return the game launcher_focus_game_set last named.
 *
 * @return The game id, "" for none. */
const char* launcher_focus_game(void);

/** @brief Queue a launch-time job of a game's library entry, run when no other job runs: its module check (and rebuild) and its asset cache check (and extraction).
 *
 * @param g The game's per-game entry.
 * @param entry_dir The library entry's directory.
 * @param disc The entry's disc.cue.
 * @param sha The entry's content hash, as its meta.toml records it.
 * @param module false to skip the module check (the entry holds a failed-build marker of this build). */
void launcher_job_add(const openpete_launcher_game* g, const char* entry_dir, const char* disc, const char* sha,
                      bool module);

/** @brief Start a game's asset job (openpete_launcher_game.update_assets) for a library entry: on the ingest thread while screens show, inline without them.
 *
 * The game must be installed and counted in flight by the caller; nothing
 * starts while another asset job is busy or for a game without assets. The
 * outcome is logged when the launcher's thread reaps it (the moment its
 * assets are ready), from the job runner, a task's wait or
 * launcher_assets_wait.
 * @param g The game's per-game entry.
 * @param disc The entry's disc.cue.
 * @param sha The entry's content hash.
 * @param context What starts it: "ingest" or "pick". */
void launcher_assets_start(const openpete_launcher_game* g, const char* disc, const char* sha, const char* context);

/** @brief Wait for the busy asset job to end and reap it, keeping the window's events drained. */
void launcher_assets_wait(void);

/** @brief On the launcher's thread: reap a finished job (writing or clearing its entry's failed-build marker) and start the next queued one.
 *
 * Starting a job installs its game (openpete_launcher_game.install), counts
 * it in flight, pauses the launcher's pipeline compile, runs its asset part
 * on the openpete-ingest thread and its module part on the openpete-job
 * thread: the stamped module path is computed there and a missing module
 * built. The job ends when both parts have. Without screens nothing starts
 * here (launcher_jobs_run_inline). */
void launcher_jobs_pump(void);

/** @brief Run every queued job on the calling thread, in order, each with its game installed, its asset part inside the `ingest` role before its module part: the form without screens. */
void launcher_jobs_run_inline(void);

/** @brief Stop the running job unless it is of a game, waiting for its module part to kill its compiler command and its asset part to stop between levels.
 *
 * @param keep_game The game whose job keeps running, or NULL to stop any.
 * @param requeue Put a job whose module or asset part stopped back at the head of the queue. */
void launcher_jobs_stop_other(const char* keep_game, bool requeue);

/** @brief Drop a game's queued jobs and wait for its running one, drawing that job's progress while screens show.
 *
 * @param game The game id. */
void launcher_jobs_finish_game(const char* game);

/** @brief Report whether no launcher job runs and none is queued.
 *
 * @return true when idle. */
bool launcher_jobs_idle(void);

/** @brief Count a module build the launcher runs itself (the pick's) as a job in flight, from its start to its end.
 *
 * @param delta +1 at the start, -1 at the end. */
void launcher_jobs_token(int delta);

/** @brief Report whether a library entry holds a failed-build marker naming this executable's build id.
 *
 * @param entry_dir The entry's directory.
 * @return true for a current marker. */
bool library_marker_current(const char* entry_dir);

/** @brief Write a library entry's failed-build marker: this executable's build id and the step that failed.
 *
 * @param entry_dir The entry's directory.
 * @param step The step's text (the last progress stage reported). */
void library_marker_write(const char* entry_dir, const char* step);

/** @brief Remove a library entry's failed-build marker, if any.
 *
 * @param entry_dir The entry's directory. */
void library_marker_clear(const char* entry_dir);

/** @brief Report whether launcher screens may show now: a windowed run before the hand-off.
 *
 * @return true when they may. */
bool launcher_screens(void);

/** @brief Ingest a disc dropped on a screen while no game is installed, build its game's module, and return, the screens then showing the library again.
 *
 * @param path The dropped image. */
void library_ingest_drop(const char* path);

/** @brief Release the library lock (openpete_launcher_lock). */
void library_lock_release(void);

/** @brief The last progress stage reported, for a failed-build marker's step.
 *
 * @return The stage text, "" before the first report; never NULL. */
const char* launcher_last_stage(void);

/** @brief Run the library scan on a worker while the launcher shows its pending screen.
 *
 * With screens (a windowed run before the hand-off) the scan starts on a
 * worker thread, then the launcher's window and device are created and the
 * pending screen (the title and a reading line) presents until the scan
 * returns; a quit or a dropped disc during the wait is returned for the
 * caller to act on once the scan is done. A device that cannot be created
 * is reported by the next launcher_open that knows whether the run can
 * continue. Without screens, or when the worker cannot start, @p fn runs on
 * the calling thread. @p fn must not touch SDL video or the launcher's
 * screens.
 * @param fn The scan.
 * @param userdata Its argument.
 * @return 0, LAUNCHER_SEL_QUIT or LAUNCHER_SEL_DROPPED. */
int launcher_await_scan(openpete_launcher_task_fn fn, void* userdata);

/** @brief The disc image path of the last drop or file-dialog choice.
 *
 * @return The path, "" before the first; never NULL. */
const char* launcher_dropped_path(void);

/** @brief Resolve this boot's disc and module path through the library.
 *
 * An entry already booted in this process (the pick made before a game was
 * installed) boots again, without a scan or a stamp, when the options name
 * no image. --ingest copies the image in (on a launcher task, with
 * progress) and boots its entry; --disc or --exe-path bypasses the
 * library; otherwise the entries recorded for this game (every game's
 * while the launcher names none), and the unidentified ones, are scanned.
 * The selector shows for several entries, and for any number while the
 * launcher names no game; one entry of a named game boots without it, and
 * without a screen the most recently played boots. A disc dropped on the
 * selector, or the scripted drop, is ingested and boots instead. Booting points opts->disc_path at the entry's
 * disc.cue, writes the module path, and stamps the entry played unless it
 * is another game's.
 * @param opts The options.
 * @param module_path Receives the module path, "" for none; NULL for none.
 * @param module_cap Its capacity.
 * @return 0 on a boot, a bypass or an empty library, else a
 *         LAUNCHER_LIB_* value. */
int library_resolve(openpete_options_t* opts, char* module_path, size_t module_cap);

/** @brief Start the element manifest of the frame being drawn: drop the previous frame's elements.
 *
 * Called at the start of each launcher frame's draw; the frame's draws then
 * record their elements with launcher_manifest_item. Safe to call whether or
 * not OPENPETE_LAUNCHER_MANIFEST is set. */
void launcher_manifest_begin(void);

/** @brief Record one element of the frame being drawn under the reference layout's id for it.
 *
 * @param id The element's id (tests/launcher/layout-reference.json).
 * @param kind Its kind: text, rect, bar, and the other kinds the reference layout names.
 * @param x0 Left edge in window pixels.
 * @param y0 Top edge.
 * @param x1 Right edge.
 * @param y1 Bottom edge. */
void launcher_manifest_item(const char* id, const char* kind, float x0, float y0, float x1, float y1);

/** @brief Write the drawn frame's elements to <base>.<screen>.json when OPENPETE_LAUNCHER_MANIFEST=<base> is set, once per screen tag.
 *
 * The file holds the screen tag, the window size in pixels and each element
 * recorded since launcher_manifest_begin, by id, kind and rect; the write
 * replaces the file atomically and is logged with the element count.
 * @param tag The screen's tag index, below 32; a second call with a tag
 *            already written does nothing.
 * @param screen The screen's tag name, used in the file name.
 * @param w The window width in pixels the frame was drawn at.
 * @param h The window height. */
void launcher_manifest_write(unsigned tag, const char* screen, float w, float h);

/** @brief Offer a game's glyph models for the 3D title: read them from the game's launcher asset cache and switch the title to them.
 *
 * The title belongs to the game last offered with @p prefer; another game's
 * offer switches only while no 3D title shows. A game whose cache holds no
 * glyph models leaves the title as it is. Launcher thread only.
 * @param g The game's launcher entry (per-game or the launcher's own).
 * @param prefer Make the title this game's: the most recently played entry
 *        at launch, an ingest's or a pick's game.
 * @param why The reason, for the log line. */
void launcher_title_offer(const openpete_launcher_game* g, bool prefer, const char* why);

/** @brief Emit the 3D title for the present about to be drawn on the launcher's device and hand it to the title pass.
 *
 * @param win_w The window width the screens lay out in.
 * @param win_h The window height.
 * @param x0 The title's left edge, window pixels.
 * @param cy Its vertical centre.
 * @param width Its width.
 * @param gap The gap after each glyph but the last.
 * @return true when the present draws the 3D title (the pixel title is then
 *         not drawn). */
bool launcher_title_prepare(float win_w, float win_h, float x0, float cy, float width, float gap);

/** @brief Report whether a game's 3D title is chosen: a game's glyph models were read for it (launcher_title_offer).
 *
 * @return true when one is chosen. */
bool launcher_title_chosen(void);

/** @brief Report whether the present being drawn draws the 3D title, and its rect.
 *
 * @param rect Receives x0, y0, x1, y1 in window pixels (the width fitted
 *        and the cap height about the centre) when it does; may be NULL.
 * @return true when it does. */
bool launcher_title_drawn(float rect[4]);

/* launcher_page_draw and launcher_page_activate results besides an entry index. */
#define LAUNCHER_PAGE_NONE   (-1)   /* no box was activated */
#define LAUNCHER_PAGE_BROWSE (-2)   /* a box without a library disc was activated: choose a disc */

/** @brief Draw the game page into the current imgui window: one box per game the launcher's descriptor marks on_page, in its frame, with its picture, else the fill and title logo of a game with a library entry, else the empty-box drawing, the focused box grown; and record its elements in the element manifest.
 *
 * Boxes are indexed left to right. Each box belongs to the first entry of
 * its game in @p entry_games, the most recently played when the entries come
 * most recent first. The hand cursor shows over a box. The first present
 * after launcher_page_closed shows every box at its target size; later
 * presents ease the growth toward it. A box's title logo is read through
 * openpete_launcher_game.load_logo until the cache holds one.
 * @param entry_games Each library entry's game id ("" when unidentified); NULL with @p count 0 for none.
 * @param count The entry count.
 * @param focus The focused box.
 * @param hovered Receives the box under the pointer, -1 for none.
 * @return The clicked box's activation (launcher_page_activation), or LAUNCHER_PAGE_NONE. */
int launcher_page_draw(const char* const* entry_games, int count, int focus, int* hovered);

/** @brief Count the game page's boxes.
 *
 * @param entry_games As launcher_page_draw's.
 * @param count The entry count.
 * @return The box count. */
int launcher_page_box_count(const char* const* entry_games, int count);

/** @brief Return what activating a box does: the index of the library entry owning it, or LAUNCHER_PAGE_BROWSE for a box without one (choose a disc).
 *
 * @param entry_games As launcher_page_draw's.
 * @param count The entry count.
 * @param box The box.
 * @return The entry index, or LAUNCHER_PAGE_BROWSE (also for a box out of range). */
int launcher_page_activation(const char* const* entry_games, int count, int box);

/** @brief Return the game of a box on the game page.
 *
 * @param box The box.
 * @return The game id, "" for a box out of range. */
const char* launcher_page_box_game(int box);

/** @brief Return the box of a game on the game page.
 *
 * @param entry_games As launcher_page_draw's.
 * @param count The entry count.
 * @param game The game id.
 * @return The box, or -1 when the page draws none for it. */
int launcher_page_box_of_game(const char* const* entry_games, int count, const char* game);

/** @brief Report whether a library entry owns a box on the game page: its game has a box and no entry before it is of that game.
 *
 * @param entry_games Each library entry's game id, most recently played first.
 * @param i The entry's index.
 * @return true when entry @p i owns its game's box. */
bool launcher_page_owns_box(const char* const* entry_games, int i);

/** @brief Note that the page stopped showing: its next present snaps every box to its target size. */
void launcher_page_closed(void);

/** @brief Report whether a box's growth had not settled at the page's last present.
 *
 * @return true while a box animates. */
bool launcher_page_animating(void);

/** @brief Give a game's box its picture, drawn cover-cropped inside the frame in place of its fill.
 *
 * Copies the pixels into a texture the page owns, replacing any earlier
 * picture. Launcher thread, with the launcher's imgui context current.
 * @param game The game id.
 * @param rgba width * height * 4 bytes, rows top to bottom.
 * @param w The width.
 * @param h The height. */
void launcher_page_set_picture(const char* game, const unsigned char* rgba, int w, int h);

/** @brief Return the box pictures' generation: a count of openpete_launcher_picture_changed calls.
 *
 * @return The count. */
unsigned launcher_picture_gen(void);

/** @brief Hand the page's textures to the renderer backend to destroy at its next render: the hand-off, after which no page shows. */
void launcher_page_release(void);

/** @brief Push the body font at a size whose capitals match ProggyClean's at @p px, or the current font at @p px when the body font is not loaded.
 *
 * @param px The text size in window pixels, as ProggyClean would be drawn. */
void launcher_push_body_font(float px);

struct ImFont;

/** @brief Return the launcher's body font, Fredoka Medium, adding it to the current imgui context on the first call.
 *
 * The font is embedded in the executable. The first call with an imgui
 * context adds imgui's default font first when the atlas is empty and names
 * the default font io.FontDefault, so the body font is never the default the
 * dev overlay and the pixel title draw with; it logs the body font's name
 * and the default font's. A failed add is logged and not retried.
 * @return The font, or NULL without an imgui context or when it could not
 *         be added. */
ImFont* launcher_body_font(void);

#endif /* OPENPETE_LAUNCHER_INTERNAL_H */
