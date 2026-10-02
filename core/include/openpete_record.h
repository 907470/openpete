// core/include/openpete_record.h - `--record <path>` gameplay video capture.
//
// True-VFR capture: one encoded frame per game-presented frame, timestamped
// with the wall-clock instant of capture, so the output plays back at real
// game cadence (drops manifest as fewer frames per wall-second). The encoder
// links libav* in-process and writes per-frame PTS directly; there is no
// nominal frame rate.
//
// Frames are read from PsyCross's present FBO at internal resolution, cropped
// to the canonical PSX visible region (no letter/pillarbox bars) - the same
// source the native-resolution screenshot path uses. Capture dimensions are
// therefore window-independent; they are locked on the first captured frame.

#ifndef OPENPETE_RECORD_H
#define OPENPETE_RECORD_H

#include <stdint.h>
#include "openpete.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Arm recording from the parsed options.
 *
 * Stores the path, encoder, pixel format, crf, gop, audio encoder and
 * --record-frames budget; the encoder and the readback rings are built on
 * the first captured present, when the dimensions are known. A positive
 * --record-cfr selects constant-rate capture at the nearest whole multiple
 * of the tick rate and disables audio. No effect when opts->record_path is
 * NULL. In a build without libav a requested recording logs a warning and
 * stays disabled.
 * @param opts Parsed options; its strings must outlive the recording.
 * @return 0 in every current path (armed, disabled or unsupported); a
 *         non-zero value is a fatal setup error the caller reports as
 *         OPENPETE_EXIT_SETUP_ERROR. */
int  openpete_record_init(const openpete_options_t* opts);

/** @brief Queue an asynchronous readback of the PsyCross present FBO for encoding.
 *
 * Called once per PsyCross present, before PsyX_EndScene, on the thread
 * that owns the GL context. The first call builds the encoder from the
 * present FBO's dimensions (rows bottom-up) and starts the encoder thread;
 * the first call's timestamp is t=0 under variable-rate capture. When the
 * PBO ring is full its oldest completed read is handed to the encoder ring
 * first. A present whose size differs from the locked size is dropped and
 * counted, with one warning. No effect when recording is disabled or
 * failed, or when the present FBO does not exist yet.
 * @param pts_us Wall-clock capture time in microseconds from a monotonic
 *        origin. */
void openpete_record_capture_frame(uint64_t pts_us);

/** @brief Report whether this build can encode video.
 *
 * A build-time constant, safe to call before openpete_record_init. It is 0
 * for targets built without libav (OPENPETE_ENABLE_RECORD=OFF, or a
 * cross-compile with no target FFmpeg), where --record only warns and
 * every recording knob is inert; the overlay then hides its Recording rows.
 * @return 1 with libav, 0 without. */
int  openpete_record_supported(void);

/** @brief Report whether recording is armed, not failed, and still accepting frames.
 *
 * The native renderer gates its offscreen readback on this. It turns 0
 * once a --record-frames budget is spent.
 * @return Non-zero while recording accepts frames. */
int  openpete_record_active(void);

/** @brief Report whether a --record-frames budget has been filled.
 *
 * The host loop polls this and ends the run on the present that fills the
 * budget, so the clip is neither short nor padded with later presents.
 * @return Non-zero once the budget is filled; 0 when unlimited or not
 *         recording. */
int  openpete_record_finished(void);

/** @brief Copy a CPU RGBA image into the encoder ring.
 *
 * The native renderer's entry point: it reads its offscreen back and hands
 * the pixels here, so record.c holds no Vulkan code. The copy is
 * synchronous; the encoder thread encodes it. The first call builds the
 * encoder at this size (floored to even) and row order. An image whose
 * size differs from the locked size is dropped and counted, with one
 * warning. Under variable-rate capture a full ring drops the image; under
 * constant-rate capture the call waits for a free slot and the image's
 * ordinal replaces @p pts_us. No effect when recording is disabled or
 * failed, or when @p rgba is NULL or a dimension is not positive.
 * @param rgba Pixels, 4 bytes each, rows packed at @p w.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param top_down 1 when row 0 is the top row (the Vulkan offscreen), 0
 *        when bottom-up.
 * @param pts_us Wall-clock capture time in microseconds. */
void openpete_record_submit_frame(const uint8_t* rgba, int w, int h,
                                  int top_down, uint64_t pts_us);

/** @brief Finish the recording and release every resource.
 *
 * Drains the in-flight PBO reads into the encoder ring, stops and joins
 * the encoder thread after it empties the ring, flushes the video and
 * audio encoders, writes the container trailer, logs the captured and
 * dropped counts, and frees everything. After a fatal error the trailer is
 * not written. When no present was captured no file is written and a
 * warning is logged. Safe to call when recording was never armed. */
void openpete_record_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RECORD_H */
