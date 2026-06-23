// core/include/openpete_record.h — `--record <path>` gameplay video capture.
//
// True-VFR capture: one encoded frame per game-presented frame, timestamped
// with the wall-clock instant of capture, so the output plays back at real
// game cadence (drops manifest as fewer frames per wall-second). The encoder
// links libav* in-process and writes per-frame PTS directly; there is no
// nominal frame rate. See notes/plans/openpete-record-video.md.
//
// Frames are read from PsyCross's present FBO at internal resolution, cropped
// to the canonical PSX visible region (no letter/pillarbox bars) — the same
// source the native-resolution screenshot path uses. Capture dimensions are
// therefore window-independent; they are locked on the first captured frame.

#ifndef OPENPETE_RECORD_H
#define OPENPETE_RECORD_H

#include <stdint.h>
#include "openpete.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arm recording from the parsed options. No-op (returns 0) when
 * opts->record_path is NULL. The encoder and GPU-readback rings are NOT
 * built here — dimensions aren't known until the first frame renders — so
 * this only validates/stashes the encoder parameters. Returns 0 on success
 * (armed or disabled); non-zero on a fatal setup error the caller should
 * surface as OPENPETE_EXIT_SETUP_ERROR. */
int  openpete_record_init(const openpete_options_t* opts);

/* Capture the just-rendered scene at wall-clock `pts_us` (microseconds from
 * an arbitrary monotonic origin; the first call's value becomes t=0). Call
 * once per presented game frame, immediately before PsyX_EndScene. No-op if
 * recording is disabled. The first call lazily builds the encoder from the
 * present FBO's native dimensions and spawns the encoder thread. */
void openpete_record_capture_frame(uint64_t pts_us);

/* True while recording is armed and not failed — gate the (non-trivial) native
 * offscreen readback on this so it only runs when actually recording. */
int  openpete_record_active(void);

/* Graphics-API-agnostic frame submit for the native renderer: hand a CPU RGBA
 * buffer (w x h, `top_down` = 1 for the Vulkan offscreen) at wall-clock `pts_us`.
 * Lazy-builds the encoder + locks even dims on the first frame; copies into the
 * encoder ring (the encoder thread encodes off the host fiber). Keeps record.c
 * free of any Vulkan/GL knowledge. No-op if recording is disabled. */
void openpete_record_submit_frame(const uint8_t* rgba, int w, int h,
                                  int top_down, uint64_t pts_us);

/* Flush the encoder, write the container trailer, and tear everything down.
 * Safe to call unconditionally (no-op if recording was never armed or no
 * frame was ever captured). */
void openpete_record_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RECORD_H */
