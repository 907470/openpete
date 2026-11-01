/* launcher_progress.h - the progress view's step line and bar position.
 *
 * The work reports a stage by its stage id (openpete_launcher_progress: the
 * upper-case texts the ingest copy, the first-boot build, the mod compile
 * and the shader precompile pass), which the logs, the failed-build marker
 * and the quit points name. The view draws each id as its step name in
 * sentence case, a counted step followed by (done/total). The first boot's
 * stages (copy, read, translate, build, link) share one bar whose segments
 * are weighted by their measured share of the first boot's time, each
 * segment filled by its own count (MiB copied, binaries translated, KiB of
 * source compiled); every other stage fills a bar of its own by its count.
 * Launcher thread only. */
#ifndef OPENPETE_LAUNCHER_PROGRESS_H
#define OPENPETE_LAUNCHER_PROGRESS_H

#include <cstddef>

/* The first boot's segments, in the order the work runs them. */
enum class LauncherSegment { None = -1, Copy = 0, Read, Translate, Build, Count };

/** @brief Return the first-boot segment a stage id belongs to: copy, read, translate, or build (which holds the link).
 *
 * @param stage The stage id.
 * @return The segment, or LauncherSegment::None for a stage outside the first boot. */
LauncherSegment launcher_progress_segment(const char* stage);

/** @brief Return a first-boot segment's share of the bar: its share of the first boot's time measured on the release packages of the three games.
 *
 * @param seg The segment.
 * @return The share, 0..1; the four shares sum to 1. 0 for LauncherSegment::None. */
float launcher_progress_weight(LauncherSegment seg);

/** @brief Write a stage's step line: its step name in sentence case, then (done/total) on a counted step, or (done) while the total is 0 and done is not.
 *
 * A stage id the table does not name is written as given.
 * @param stage The stage id.
 * @param done Completed units.
 * @param total Total units; 0 when unknown.
 * @param out Receives the line.
 * @param cap The capacity of @p out. */
void launcher_progress_line(const char* stage, int done, int total, char* out, size_t cap);

/** @brief Return the bar's filled fraction for a stage, with or without the copy segment at the bar's start.
 *
 * A first-boot stage fills the bar to the weights of the segments before
 * its own plus its own weight times done/total (0 while total is 0; 1 on
 * the link step), over the weights of the segments the run holds: all four
 * with @p with_copy, the last three without (a rebuild reads the library's
 * copy). Any other stage fills done/total, 0 while total is 0. Clamped to
 * 0..1.
 * @param stage The stage id.
 * @param done Completed units.
 * @param total Total units.
 * @param with_copy true when the run started with the copy.
 * @return The fraction. */
float launcher_progress_frac(const char* stage, int done, int total, bool with_copy);

/** @brief Return the bar's filled fraction for the stage a progress view draws, tracking whether the first boot it belongs to started with the copy.
 *
 * The copy step marks the run as one with the copy; a read step that does
 * not follow a copy step (a rebuild) marks it as one without. With the
 * harness variable OPENPETE_LAUNCHER_PROGRESS_LOG set, each change of the
 * stage or of the bar's whole percent logs one line "progress bar: F
 * at T s (STAGE DONE/TOTAL)", T in seconds from process start.
 * @param stage The stage id.
 * @param done Completed units.
 * @param total Total units.
 * @return The fraction (launcher_progress_frac). */
float launcher_progress_bar(const char* stage, int done, int total);

#endif /* OPENPETE_LAUNCHER_PROGRESS_H */
