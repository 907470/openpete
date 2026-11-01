/* launcher_quit.h - quitting while the launcher works (launcher_quit.cpp):
 * the progress views' quit gesture and the process-wide quit that stops
 * every launcher job, task and compiler child. */
#ifndef OPENPETE_LAUNCHER_QUIT_H
#define OPENPETE_LAUNCHER_QUIT_H

union SDL_Event;

/* What one event does to the quit on a progress view. */
enum class QuitGesture {
    None,    /* not a quit gesture */
    Armed,   /* the first East: the step chip asks for a second press */
    Quit     /* the quit is taken (launcher_quit_take) */
};

/** @brief Read one event on a progress view as the quit gesture: closing the window, a quit event (SIGTERM) or Escape quits at once; East arms the quit and a second East within LAUNCHER_QUIT_ASK_MS quits.
 *
 * Every progress view's event drain routes its events here, so a change to
 * the gesture changes it on every view.
 * @param e The event.
 * @return What the event did. */
QuitGesture launcher_quit_gesture(const SDL_Event& e);

/** @brief Report whether a first East armed the quit less than LAUNCHER_QUIT_ASK_MS ago; an expired arming is cleared and logged.
 *
 * @return true while the quit chip ("Press again to quit") takes the step chip's place. */
bool launcher_quit_armed(void);

/** @brief Take the quit: hide the launcher's window and stop every launcher job, task and stoppable compiler child.
 *
 * openpete_launcher_job_cancelled reads the quit on every thread once taken.
 * A second call does nothing. Call on the launcher's thread.
 * @param how The gesture, for the log. */
void launcher_quit_take(const char* how);

/** @brief Report whether the quit was taken (launcher_quit_take).
 *
 * @return true once taken; safe on any thread. */
bool launcher_quit_taken(void);

/** @brief Log how long after the quit the work stopped, when a quit was taken; called as the launcher closes. */
void launcher_quit_report(void);

/** @brief Harness: perform the gesture OPENPETE_LAUNCHER_QUIT_AT names once its work point is reached.
 *
 * OPENPETE_LAUNCHER_QUIT_AT=<point>[+<ms>]:<gesture>, the point one of copy,
 * read, translate, build, link (the progress stage the work reports), mods
 * (the mod compile) or assets (a wait for an asset job), and the gesture
 * one of quit (a quit event), sigterm (the signal, raised in the process),
 * close (a window close request), escape, east (one press) or east2 (two
 * presses 500 ms apart). The gesture is posted as
 * input events <ms> after the point is first seen. Call on the launcher's
 * thread.
 * @param mark The work point the caller is waiting on, or NULL to take it
 *        from the last progress stage. */
void launcher_quit_script_poll(const char* mark);

#endif /* OPENPETE_LAUNCHER_QUIT_H */
