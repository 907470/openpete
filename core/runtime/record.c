// core/runtime/record.c — `--record <path>` true-VFR gameplay video capture.
//
// One encoded frame per game-presented frame, PTS = wall-clock capture time,
// so playback reflects real game cadence: a load-induced drop to 24 fps shows
// up as 24 frames in that wall-second, not a re-timed 30. There is no nominal
// frame rate — the muxer stream time_base is microseconds and each frame
// carries its own PTS.
//
// Pipeline (see notes/plans/openpete-record-video.md):
//
//   [host fiber / GL thread]            [encoder thread]
//   -------------------------           ------------------
//   capture_frame(pts_us):              loop:
//     GR_ReadPresentSceneRGBA  ---ring--> sws RGBA->yuv420p (flip via -stride)
//     -> preallocated slot                avcodec_send_frame (pts = us - start)
//     publish + signal                    drain receive_packet
//                                         av_interleaved_write_frame
//
// The frame source is the present FBO at internal resolution, cropped to the
// canonical PSX visible region (no letter/pillarbox bars) — the same source
// the native-resolution screenshot path reads. Dimensions are locked on the
// first captured frame; a later window resize that changes the native size is
// dropped (counted + warned) for now — see the plan's robustness phase.
//
// libav* is a hard build dependency (the user opted into hard-require), so
// there is no graceful no-op-when-absent path; missing libav fails the build.

#include "openpete_record.h"
#include "openpete_log.h"

#include <SDL3/SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PsyCross present-FBO readers (declared in PsyX/PsyX_render.h, but that
 * header pulls in GL — forward-declare here to keep this TU GL-free). All
 * return 0/NULL when there is no present FBO. Pixel rows are bottom-up.
 *
 * Capture uses the async PBO ring (GR_PresentPBO_*) so the GPU->CPU readback
 * overlaps rendering instead of stalling the GL thread; GR_GetPresentSceneSize
 * is still used to learn the native dimensions. */
extern int GR_GetPresentSceneSize(int* out_w, int* out_h);
extern int GR_PresentPBO_RingDepth(void);
extern int GR_PresentPBO_Pending(void);
extern int GR_PresentPBO_Submit(unsigned int gl_format, int* out_w, int* out_h);
extern unsigned char* GR_PresentPBO_Map(int* out_w, int* out_h);
extern void GR_PresentPBO_Unmap(void);

/* glReadPixels format token. Spelled out so this TU needn't include glad.h;
 * GL_RGBA is a stable GL enum. */
#define OPENPETE_GL_RGBA 0x1908u

/* Preallocated frame ring. 32 slots at internal-res RGBA (~40 MB at
 * 640x480-equivalent; more at ultrawide) absorbs encoder jitter without
 * stalling the game thread. Full ring => drop (game stays real-time). */
#define REC_RING_SLOTS 32

static struct {
    int          enabled;       /* --record path was given */
    int          initialized;   /* encoder built (dims known) */
    int          failed;        /* fatal error; stop trying */

    /* Encoder parameters (copied from opts; opts strings outlive us). */
    const char*  path;
    const char*  encoder_name;
    const char*  pixfmt_name;
    int          crf;
    int          gop;

    /* Locked dimensions. native_{w,h} is the present-FBO size we read each
     * frame; {w,h} is the encoder geometry, the native size floored to even
     * (yuv420p chroma subsampling requires even width/height, and widened
     * ultrawide widths come out odd). The bottom-left w x h sub-rectangle is
     * encoded; at most one row/column is dropped. */
    int          native_w, native_h;
    int          w, h;

    /* libav* */
    AVFormatContext* fmt;
    AVCodecContext*  codec;
    AVStream*        st;
    struct SwsContext* sws;
    AVFrame*         frame;
    AVPacket*        pkt;

    /* Frame ring (RGBA, bottom-up). */
    uint8_t*     slots[REC_RING_SLOTS];
    uint64_t     pts_us[REC_RING_SLOTS];
    int          write_idx;
    int          read_idx;
    int          count;         /* occupied slots, incl. in-flight read */
    SDL_Mutex*   mutex;
    SDL_Condition* cond;
    SDL_Thread*  thread;
    int          stop;          /* set under mutex to wake + drain thread */

    /* PTS FIFO parallel to the PsyCross PBO ring: capture timestamps of
     * submitted-but-not-yet-retrieved frames, in submit order. Paired with
     * pixels at Map time so each frame keeps its true draw timestamp despite
     * the few-frame readback latency. Depth = GR_PresentPBO_RingDepth(). */
#define REC_PBO_PTS_MAX 16
    uint64_t     pbo_pts[REC_PBO_PTS_MAX];
    int          pbo_pts_tail;
    int          pbo_pts_count;
    int          pbo_depth;

    /* Stats */
    uint64_t     start_us;      /* first capture timestamp (t=0) */
    uint64_t     captured;
    uint64_t     dropped;
    int          warned_resize;
} g_rec;

/* ---- encode one frame (encoder thread, and main thread during flush) ---- */

/* Send `buf` (RGBA, bottom-up, native_w x native_h) or NULL to flush, then
 * drain and mux any produced packets. Runs only on the encoder thread while
 * the loop is live, and on the main thread after the thread is joined — never
 * concurrently. Returns 0 on success, negative on a fatal encode error. */
static int rec_encode_one(const uint8_t* buf, uint64_t pts_us) {
    if (buf) {
        if (av_frame_make_writable(g_rec.frame) < 0)
            return -1;
        /* Encode the bottom-left g_rec.w x g_rec.h sub-rectangle of the
         * native buffer. Rows are bottom-up (GL origin): start at native row
         * (h-1) and walk down with a negated stride to flip to top-down, so
         * the encoder reads native rows [0..h-1] (the bottom h rows). The
         * row pitch is the *native* width; sws takes only w pixels per row. */
        const size_t native_pitch = (size_t)g_rec.native_w * 4u;
        const uint8_t* src[4] = {
            buf + (size_t)(g_rec.h - 1) * native_pitch, NULL, NULL, NULL
        };
        const int src_stride[4] = { -(int)native_pitch, 0, 0, 0 };
        sws_scale(g_rec.sws, src, src_stride, 0, g_rec.h,
                  g_rec.frame->data, g_rec.frame->linesize);
        g_rec.frame->pts = (int64_t)(pts_us - g_rec.start_us);
    }

    int ret = avcodec_send_frame(g_rec.codec, buf ? g_rec.frame : NULL);
    if (ret < 0) {
        OPL_ERROR_F("record", "avcodec_send_frame failed: %s", av_err2str(ret));
        return -1;
    }
    for (;;) {
        ret = avcodec_receive_packet(g_rec.codec, g_rec.pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            OPL_ERROR_F("record", "avcodec_receive_packet failed: %s",
                        av_err2str(ret));
            return -1;
        }
        av_packet_rescale_ts(g_rec.pkt, g_rec.codec->time_base,
                             g_rec.st->time_base);
        g_rec.pkt->stream_index = g_rec.st->index;
        ret = av_interleaved_write_frame(g_rec.fmt, g_rec.pkt);
        av_packet_unref(g_rec.pkt);
        if (ret < 0) {
            OPL_ERROR_F("record", "write_frame failed: %s", av_err2str(ret));
            return -1;
        }
    }
}

static int rec_thread_main(void* unused) {
    (void)unused;
    for (;;) {
        SDL_LockMutex(g_rec.mutex);
        while (g_rec.count == 0 && !g_rec.stop)
            SDL_WaitCondition(g_rec.cond, g_rec.mutex);
        if (g_rec.count == 0 && g_rec.stop) {
            SDL_UnlockMutex(g_rec.mutex);
            break;
        }
        const int idx = g_rec.read_idx;
        const uint64_t pts = g_rec.pts_us[idx];
        uint8_t* buf = g_rec.slots[idx];
        SDL_UnlockMutex(g_rec.mutex);

        /* Slot stays counted (producer won't reuse it) until we free it
         * below — so encoding from `buf` outside the lock is safe. */
        if (rec_encode_one(buf, pts) < 0)
            g_rec.failed = 1;

        SDL_LockMutex(g_rec.mutex);
        g_rec.read_idx = (idx + 1) % REC_RING_SLOTS;
        g_rec.count--;
        SDL_UnlockMutex(g_rec.mutex);
    }
    return 0;
}

/* ---- lazy encoder build (first captured frame) ---- */

static void rec_teardown(void);  /* fwd */

static int rec_lazy_init(int native_w, int native_h, uint64_t first_pts_us) {
    g_rec.native_w = native_w;
    g_rec.native_h = native_h;
    g_rec.pbo_depth = GR_PresentPBO_RingDepth();
    if (g_rec.pbo_depth > REC_PBO_PTS_MAX) g_rec.pbo_depth = REC_PBO_PTS_MAX;
    g_rec.pbo_pts_tail = 0;
    g_rec.pbo_pts_count = 0;
    /* yuv420p (and h264 generally) needs even dimensions; ultrawide widening
     * yields odd widths. Floor to even — drops at most one edge row/column. */
    const int w = native_w & ~1;
    const int h = native_h & ~1;
    g_rec.w = w;
    g_rec.h = h;
    g_rec.start_us = first_pts_us;
    if (w <= 0 || h <= 0) {
        OPL_ERROR_F("record", "degenerate capture size %dx%d", native_w, native_h);
        return -1;
    }

    int ret = avformat_alloc_output_context2(&g_rec.fmt, NULL, NULL, g_rec.path);
    if (ret < 0 || !g_rec.fmt) {
        OPL_ERROR_F("record", "avformat_alloc_output_context2(%s) failed: %s",
                    g_rec.path, av_err2str(ret));
        return -1;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(g_rec.encoder_name);
    if (!codec) {
        OPL_ERROR_F("record", "unknown encoder '%s'", g_rec.encoder_name);
        return -1;
    }

    enum AVPixelFormat pixfmt = av_get_pix_fmt(g_rec.pixfmt_name);
    if (pixfmt == AV_PIX_FMT_NONE) {
        OPL_ERROR_F("record", "unknown pixfmt '%s'", g_rec.pixfmt_name);
        return -1;
    }

    g_rec.st = avformat_new_stream(g_rec.fmt, NULL);
    if (!g_rec.st) {
        OPL_ERROR_F("record", "avformat_new_stream failed");
        return -1;
    }

    g_rec.codec = avcodec_alloc_context3(codec);
    if (!g_rec.codec) {
        OPL_ERROR_F("record", "avcodec_alloc_context3 failed");
        return -1;
    }
    g_rec.codec->codec_id  = codec->id;
    g_rec.codec->width     = w;
    g_rec.codec->height    = h;
    g_rec.codec->pix_fmt   = pixfmt;
    g_rec.codec->gop_size  = g_rec.gop;
    /* Microsecond timestamps; true VFR (no fixed framerate). */
    g_rec.codec->time_base = (AVRational){ 1, 1000000 };
    if (g_rec.fmt->oformat->flags & AVFMT_GLOBALHEADER)
        g_rec.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* x264/x265 private options (ignored by other encoders). veryfast keeps
     * software encode feasible at high capture rates; crf sets quality. */
    if (g_rec.codec->priv_data) {
        char crf_str[16];
        snprintf(crf_str, sizeof(crf_str), "%d", g_rec.crf);
        av_opt_set(g_rec.codec->priv_data, "preset", "veryfast", 0);
        av_opt_set(g_rec.codec->priv_data, "crf", crf_str, 0);
    }

    ret = avcodec_open2(g_rec.codec, codec, NULL);
    if (ret < 0) {
        OPL_ERROR_F("record", "avcodec_open2(%s) failed: %s",
                    g_rec.encoder_name, av_err2str(ret));
        return -1;
    }

    ret = avcodec_parameters_from_context(g_rec.st->codecpar, g_rec.codec);
    if (ret < 0) {
        OPL_ERROR_F("record", "parameters_from_context failed: %s",
                    av_err2str(ret));
        return -1;
    }
    g_rec.st->time_base = g_rec.codec->time_base;

    if (!(g_rec.fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&g_rec.fmt->pb, g_rec.path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            OPL_ERROR_F("record", "avio_open(%s) failed: %s",
                        g_rec.path, av_err2str(ret));
            return -1;
        }
    }

    ret = avformat_write_header(g_rec.fmt, NULL);
    if (ret < 0) {
        OPL_ERROR_F("record", "write_header failed: %s", av_err2str(ret));
        return -1;
    }

    /* src==dst dimensions: no scaling, just RGBA -> encoder pixfmt. */
    g_rec.sws = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, pixfmt,
                               SWS_POINT, NULL, NULL, NULL);
    if (!g_rec.sws) {
        OPL_ERROR_F("record", "sws_getContext failed");
        return -1;
    }

    g_rec.frame = av_frame_alloc();
    g_rec.pkt   = av_packet_alloc();
    if (!g_rec.frame || !g_rec.pkt) {
        OPL_ERROR_F("record", "frame/packet alloc failed");
        return -1;
    }
    g_rec.frame->format = pixfmt;
    g_rec.frame->width  = w;
    g_rec.frame->height = h;
    if (av_frame_get_buffer(g_rec.frame, 0) < 0) {
        OPL_ERROR_F("record", "av_frame_get_buffer failed");
        return -1;
    }

    const size_t slot_bytes = (size_t)native_w * (size_t)native_h * 4u;
    for (int i = 0; i < REC_RING_SLOTS; ++i) {
        g_rec.slots[i] = (uint8_t*)malloc(slot_bytes);
        if (!g_rec.slots[i]) {
            OPL_ERROR_F("record", "ring slot alloc failed");
            return -1;
        }
    }

    g_rec.mutex = SDL_CreateMutex();
    g_rec.cond  = SDL_CreateCondition();
    if (!g_rec.mutex || !g_rec.cond) {
        OPL_ERROR_F("record", "mutex/cond create failed");
        return -1;
    }

    g_rec.thread = SDL_CreateThread(rec_thread_main, "openpete-record", NULL);
    if (!g_rec.thread) {
        OPL_ERROR_F("record", "encoder thread create failed");
        return -1;
    }

    g_rec.initialized = 1;
    if (w != native_w || h != native_h)
        OPL_INFO_F("record", "native %dx%d -> encoding %dx%d (even-aligned)",
                   native_w, native_h, w, h);
    OPL_INFO_F("record", "armed: %s [%s %dx%d crf=%d gop=%d %s]",
               g_rec.path, g_rec.encoder_name, w, h, g_rec.crf, g_rec.gop,
               g_rec.pixfmt_name);
    return 0;
}

/* ---- public API ---- */

int openpete_record_init(const openpete_options_t* opts) {
    memset(&g_rec, 0, sizeof(g_rec));
    if (!opts || !opts->record_path)
        return 0;  /* disabled */
    g_rec.enabled      = 1;
    g_rec.path         = opts->record_path;
    g_rec.encoder_name = opts->record_encoder ? opts->record_encoder : "libx264";
    g_rec.pixfmt_name  = opts->record_pixfmt  ? opts->record_pixfmt  : "yuv420p";
    g_rec.crf          = opts->record_crf;
    g_rec.gop          = opts->record_gop;
    return 0;
}

/* Publish one RGBA frame (native_w x native_h, bottom-up) into the encoder
 * slot ring with timestamp pts_us. Drops (and counts) if the ring is full. */
static void rec_publish(const uint8_t* rgba, uint64_t pts_us) {
    SDL_LockMutex(g_rec.mutex);
    if (g_rec.count >= REC_RING_SLOTS) {
        SDL_UnlockMutex(g_rec.mutex);
        if (g_rec.dropped == 0)
            OPL_WARN_F("record", "ring full; dropping frame(s) "
                                 "(encoder can't keep up)");
        g_rec.dropped++;
        return;
    }
    const int idx = g_rec.write_idx;
    SDL_UnlockMutex(g_rec.mutex);

    memcpy(g_rec.slots[idx], rgba,
           (size_t)g_rec.native_w * (size_t)g_rec.native_h * 4u);

    SDL_LockMutex(g_rec.mutex);
    g_rec.pts_us[idx] = pts_us;
    g_rec.write_idx   = (idx + 1) % REC_RING_SLOTS;
    g_rec.count++;
    g_rec.captured++;
    SDL_SignalCondition(g_rec.cond);
    SDL_UnlockMutex(g_rec.mutex);
}

/* Retrieve the oldest completed PBO read and hand it to the encoder ring,
 * paired with its submit-time PTS. No-op if nothing is in flight. */
static void rec_drain_one(void) {
    if (g_rec.pbo_pts_count <= 0)
        return;
    int mw = 0, mh = 0;
    unsigned char* px = GR_PresentPBO_Map(&mw, &mh);
    const uint64_t pts = g_rec.pbo_pts[g_rec.pbo_pts_tail];
    g_rec.pbo_pts_tail = (g_rec.pbo_pts_tail + 1) % g_rec.pbo_depth;
    g_rec.pbo_pts_count--;
    if (px) {
        if (mw == g_rec.native_w && mh == g_rec.native_h)
            rec_publish(px, pts);
        GR_PresentPBO_Unmap();
    }
}

void openpete_record_capture_frame(uint64_t pts_us) {
    if (!g_rec.enabled || g_rec.failed)
        return;

    int w = 0, h = 0;
    if (!GR_GetPresentSceneSize(&w, &h) || w <= 0 || h <= 0)
        return;  /* present FBO not ready yet; try next frame */

    if (!g_rec.initialized) {
        if (rec_lazy_init(w, h, pts_us) != 0) {
            OPL_ERROR_F("record", "init failed; recording disabled");
            g_rec.failed = 1;
            rec_teardown();
            return;
        }
    }

    /* Make room: when the PBO ring is full, retrieve its oldest (now
     * completed) read before issuing a new one. This is where the
     * few-frame GPU->CPU readback latency is absorbed without a stall. */
    if (GR_PresentPBO_Pending() >= g_rec.pbo_depth)
        rec_drain_one();

    if (w != g_rec.native_w || h != g_rec.native_h) {
        /* Window resized mid-record => native size changed; the encoder is
         * locked to the initial dims. Drop the new-size frame (don't submit),
         * but keep draining in-flight frames so the pipeline stays
         * consistent. (Resize-rescale is a plan robustness item.) */
        if (!g_rec.warned_resize) {
            OPL_WARN_F("record",
                       "native size changed %dx%d -> %dx%d mid-record; "
                       "dropping frames at the new size",
                       g_rec.native_w, g_rec.native_h, w, h);
            g_rec.warned_resize = 1;
        }
        g_rec.dropped++;
        return;
    }

    /* Issue an async read of this frame and remember its timestamp; the
     * pixels are retrieved a few frames later by rec_drain_one. */
    if (GR_PresentPBO_Submit(OPENPETE_GL_RGBA, NULL, NULL)) {
        const int slot = (g_rec.pbo_pts_tail + g_rec.pbo_pts_count) % g_rec.pbo_depth;
        g_rec.pbo_pts[slot] = pts_us;
        g_rec.pbo_pts_count++;
    }
}

/* Free everything (idempotent). Encoder thread must already be joined. */
static void rec_teardown(void) {
    if (g_rec.sws)   { sws_freeContext(g_rec.sws);   g_rec.sws = NULL; }
    if (g_rec.frame) { av_frame_free(&g_rec.frame); }
    if (g_rec.pkt)   { av_packet_free(&g_rec.pkt); }
    if (g_rec.codec) { avcodec_free_context(&g_rec.codec); }
    if (g_rec.fmt) {
        if (g_rec.fmt->pb && !(g_rec.fmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&g_rec.fmt->pb);
        avformat_free_context(g_rec.fmt);
        g_rec.fmt = NULL;
    }
    for (int i = 0; i < REC_RING_SLOTS; ++i) {
        free(g_rec.slots[i]);
        g_rec.slots[i] = NULL;
    }
    if (g_rec.cond)  { SDL_DestroyCondition(g_rec.cond); g_rec.cond = NULL; }
    if (g_rec.mutex) { SDL_DestroyMutex(g_rec.mutex);    g_rec.mutex = NULL; }
}

void openpete_record_shutdown(void) {
    if (!g_rec.enabled)
        return;

    if (g_rec.initialized) {
        /* Drain any in-flight PBO reads into the encoder ring before stopping
         * the encoder thread, so the last few frames aren't lost to the
         * readback latency. Runs on the GL thread (this is called from the
         * host fiber), which is required for the GL map/unmap. */
        while (GR_PresentPBO_Pending() > 0)
            rec_drain_one();

        /* Wake + join the encoder thread (drains the slot ring first). */
        SDL_LockMutex(g_rec.mutex);
        g_rec.stop = 1;
        SDL_SignalCondition(g_rec.cond);
        SDL_UnlockMutex(g_rec.mutex);
        SDL_WaitThread(g_rec.thread, NULL);
        g_rec.thread = NULL;

        /* Flush the encoder (drain delayed frames) and finalize the file,
         * unless an earlier error already poisoned the stream. */
        if (!g_rec.failed) {
            rec_encode_one(NULL, 0);
            int ret = av_write_trailer(g_rec.fmt);
            if (ret < 0)
                OPL_ERROR_F("record", "av_write_trailer failed: %s",
                            av_err2str(ret));
        }
        OPL_INFO_F("record", "finalized %s: %llu frame(s) captured, "
                             "%llu dropped",
                   g_rec.path,
                   (unsigned long long)g_rec.captured,
                   (unsigned long long)g_rec.dropped);
    } else if (!g_rec.failed) {
        OPL_WARN_F("record", "no frames captured (present FBO never ready?); "
                             "no file written");
    }

    rec_teardown();
    memset(&g_rec, 0, sizeof(g_rec));
}
