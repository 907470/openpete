// core/record/record.c - `--record <path>` true-VFR gameplay video capture.
//
// One encoded frame per game-presented frame, PTS = wall-clock capture time,
// so playback reflects real game cadence: a load-induced drop to 24 fps shows
// up as 24 frames in that wall-second, not a re-timed 30. There is no nominal
// frame rate - the muxer stream time_base is microseconds and each frame
// carries its own PTS.
//
// Pipeline:
//
//   [host fiber / GL thread]            [encoder thread]
//   -------------------------           ------------------
//   capture_frame(pts_us):              loop:
//     GR_PresentPBO_Map (async) --ring--> sws RGBA->pixfmt (flip via -stride)
//     -> preallocated slot                avcodec_send_frame (pts = us - start)
//     publish + signal                    drain receive_packet
//                                         av_interleaved_write_frame
//
// The frame source is the present FBO at internal resolution, cropped to the
// canonical PSX visible region (no letter/pillarbox bars) - the same source
// the native-resolution screenshot path reads. Dimensions are locked on the
// first captured frame; a frame from a later window resize that changes the
// native size is dropped, counted and warned once.
//
// libav* is a hard build dependency on native builds. The one exception is cross-compiling (e.g. the MinGW Windows
// preset), where no target libav exists and host pkg-config would poison the
// build; there OPENPETE_HAS_LIBAV is undefined and this TU compiles to the
// stubs at the bottom of the file (the public entry points do nothing, and
// init warns once if --record was requested). See core/CMakeLists.txt.

#include "openpete_record.h"
#include "openpete_atomic_file.h"
#include "openpete_crash.h"
#include "openpete_log.h"
#include "openpete_time.h"   /* OP_VBL_HZ_DEN / OP_VBL_HZ_NUM / OP_TICK_HZ */

#ifdef OPENPETE_HAS_LIBAV

#include <SDL3/SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PsyCross present-FBO readers (declared in PsyX/PsyX_render.h, but that
 * header pulls in GL - forward-declare here to keep this TU GL-free). All
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

/* PsyCross SPU capture ring (PsyX_SPUAL.h). CaptureActive is non-zero while
 * the host has armed it (PsyX_SPUAL_SetCapture); CaptureDrain pulls the
 * SPU's rendered stereo s16 @ 44100 frames, the same frames the device
 * feed plays. */
extern int PsyX_SPUAL_CaptureActive(void);
extern int PsyX_SPUAL_CaptureDrain(short* out, int max_frames);
extern void PsyX_SPUAL_CaptureStats(unsigned long long* produced,
                                    unsigned long long* dropped);
extern void PsyX_SPUAL_CaptureResetStats(void);

/* glReadPixels format token. Spelled out so this TU needn't include glad.h;
 * GL_RGBA is a stable GL enum. */
#define OPENPETE_GL_RGBA 0x1908u

/* Preallocated frame ring. 32 slots at internal-res RGBA (~40 MB at
 * 640x480-equivalent; more at ultrawide) absorbs encoder jitter without
 * stalling the game thread. Full ring => drop under VFR (game stays
 * real-time); under CFR the producer waits for a free slot. */
#define REC_RING_SLOTS 32

static struct {
    int          enabled;       /* --record path was given */
    int          initialized;   /* encoder built (dims known) */
    int          failed;        /* fatal error; stop trying */

    /* Encoder parameters (copied from opts; opts strings outlive us). */
    const char*  path;
    /* "<path>.part": the stream is written here and renamed over path once
     * the trailer is written; empty when the muxer opens no file. */
    char         part_path[OP_ATOMIC_PATH_MAX + 8];
    const char*  encoder_name;
    const char*  pixfmt_name;
    int          crf;
    int          gop;

    /* Audio (SPU capture ring -> AAC second stream). */
    int          audio_requested;    /* not --no-record-audio */
    int          audio_enabled;      /* requested + capture ring armed + set up */
    const char*  audio_encoder_name;
    AVStream*    astream;
    AVCodecContext* acodec;
    struct SwrContext* swr;          /* s16 interleaved -> encoder sample fmt */
    AVAudioFifo* afifo;              /* buffers captured s16 until frame_size */
    AVFrame*     aframe;             /* one encoder frame (acodec->frame_size) */
    int64_t      a_next_pts;         /* cumulative samples encoded */

    /* Locked dimensions. native_{w,h} is the present-FBO size we read each
     * frame; {w,h} is the encoder geometry, the native size floored to even
     * (yuv420p chroma subsampling requires even width/height, and widened
     * ultrawide widths come out odd). The w x h sub-rectangle anchored at
     * the bottom-left (bottom-up source) or top-left (top-down source) is
     * encoded; at most one row/column is dropped. */
    int          native_w, native_h;
    int          w, h;
    /* Source row order: 0 = bottom-up (PsyCross GL present FBO), 1 = top-down
     * (native Vulkan offscreen via rhi_read_offscreen). Set at lazy-init from
     * the first frame's source; rec_encode_one flips accordingly. */
    int          src_top_down;

    /* libav* */
    AVFormatContext* fmt;
    AVCodecContext*  codec;
    AVStream*        st;
    struct SwsContext* sws;
    AVFrame*         frame;
    AVPacket*        pkt;

    /* Frame ring (RGBA, native_w x native_h, rows in src_top_down order). */
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

    /* Constant-frame-rate capture (--record-cfr). `cfr_mult` is the number of
     * presents per NTSC sim tick (1 = 29.913 fps, 2 = 59.826, 4 = 119.652);
     * zero means true VFR. Under CFR the encoder time_base is
     * 37609/(1125000*mult), a
     * frame's PTS is simply its ordinal, and the ring applies backpressure
     * instead of dropping - a dropped frame would silently shift every later
     * frame's game time. */
    int          cfr_mult;
    uint64_t     cfr_next;      /* next CFR ordinal to hand out */
    int          cfr_started;   /* cfr_first_tag holds the first image's tag */
    uint64_t     cfr_first_tag; /* capture timestamp of ordinal 0, microseconds */
    int          cfr_prev_idx;  /* ring slot of the last image queued */
    uint64_t     cfr_filled;    /* ordinals filled by repeating the previous image */
    uint64_t     cfr_dup;       /* images dropped for an ordinal already queued */

    /* --record-frames: stop after this many encoded frames (0 = unlimited). */
    uint64_t     max_frames;
    int          finished;      /* frame budget reached; ignore further frames */

    /* Stats */
    uint64_t     start_us;      /* first capture timestamp (t=0) */
    uint64_t     captured;
    uint64_t     dropped;
    int          warned_resize;
} g_rec;

/* ---- encode one frame (encoder thread, and main thread during flush) ---- */

/** @brief Encode one ring image, or flush the video encoder, and mux every packet it produces.
 *
 * Converts the even-floored w x h sub-rectangle of the native-pitch buffer
 * to the encoder pixel format: a bottom-up source is read from row h-1
 * with a negated stride, a top-down source from row 0. The frame PTS is
 * @p pts_us minus start_us (start_us is 0 under constant-rate capture,
 * where @p pts_us is an ordinal). Runs on the encoder thread while it
 * lives and on the shutdown thread after the join, never concurrently.
 * @param buf RGBA image of native_w x native_h, or NULL to flush.
 * @param pts_us Capture timestamp in microseconds, or the constant-rate
 *        ordinal.
 * @return 0 on success, -1 on a send, receive or write error. */
static int rec_encode_one(const uint8_t* buf, uint64_t pts_us) {
    if (buf) {
        if (av_frame_make_writable(g_rec.frame) < 0)
            return -1;
        /* Encode the g_rec.w x g_rec.h sub-rectangle of the native buffer; the
         * row pitch is the *native* width, sws takes only w pixels per row.
         * Bottom-up source (PsyCross GL FBO): start at native row (h-1) and walk
         * down with a negated stride -> reads the bottom h rows top-to-bottom (the
         * visible region after overscan crop). Top-down source (native Vulkan
         * offscreen): start at row 0 with a positive stride -> reads the top h
         * rows as-is. */
        const size_t native_pitch = (size_t)g_rec.native_w * 4u;
        const uint8_t* src[4];
        int src_stride[4] = { 0, 0, 0, 0 };
        if (g_rec.src_top_down) {
            src[0] = buf;
            src_stride[0] = (int)native_pitch;
        } else {
            src[0] = buf + (size_t)(g_rec.h - 1) * native_pitch;
            src_stride[0] = -(int)native_pitch;
        }
        src[1] = src[2] = src[3] = NULL;
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

/* ---- audio (SPU capture ring -> second stream) ---- */

/** @brief Add the audio stream: encoder, s16-to-encoder resampler, and sample FIFO.
 *
 * Called from rec_lazy_init before the container header is written, since
 * every stream must exist first. Needs audio requested and the PsyCross
 * capture ring armed; the stream is 44100 Hz stereo at 160 kb/s.
 * Any failure logs a warning and leaves audio_enabled 0, so recording
 * continues with video only. On success it discards the capture backlog
 * and resets its drop counters so audio sample 0 lines up with the first
 * video image.
 * @return 0 in every path. */
static int rec_setup_audio(void) {
    if (!g_rec.audio_requested || !PsyX_SPUAL_CaptureActive())
        return 0;  /* not requested, or capture ring not armed -> video only */

    const AVCodec* ac = avcodec_find_encoder_by_name(g_rec.audio_encoder_name);
    if (!ac) {
        OPL_WARN_F("record", "audio: unknown encoder '%s'; recording video only",
                   g_rec.audio_encoder_name);
        return 0;
    }
    g_rec.astream = avformat_new_stream(g_rec.fmt, NULL);
    g_rec.acodec  = avcodec_alloc_context3(ac);
    if (!g_rec.astream || !g_rec.acodec) {
        OPL_WARN_F("record", "audio: stream/ctx alloc failed; video only");
        return 0;
    }
    g_rec.acodec->sample_rate    = 44100;
    g_rec.acodec->channels       = 2;
    g_rec.acodec->channel_layout = AV_CH_LAYOUT_STEREO;  /* legacy (ffmpeg<5.1) */
    g_rec.acodec->sample_fmt  = ac->sample_fmts ? ac->sample_fmts[0]
                                                : AV_SAMPLE_FMT_FLTP;
    g_rec.acodec->bit_rate    = 160000;
    g_rec.acodec->time_base   = (AVRational){ 1, 44100 };
    if (g_rec.fmt->oformat->flags & AVFMT_GLOBALHEADER)
        g_rec.acodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(g_rec.acodec, ac, NULL) < 0) {
        OPL_WARN_F("record", "audio: avcodec_open2(%s) failed; video only",
                   g_rec.audio_encoder_name);
        return 0;
    }
    avcodec_parameters_from_context(g_rec.astream->codecpar, g_rec.acodec);
    g_rec.astream->time_base = g_rec.acodec->time_base;

    /* Resampler: captured s16 interleaved stereo @44100 -> encoder fmt.
     * Legacy swr_alloc_set_opts (ffmpeg < 5.1 lacks the AVChannelLayout API). */
    g_rec.swr = swr_alloc_set_opts(NULL,
            AV_CH_LAYOUT_STEREO, g_rec.acodec->sample_fmt, 44100,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100, 0, NULL);
    if (!g_rec.swr || swr_init(g_rec.swr) < 0) {
        OPL_WARN_F("record", "audio: resampler init failed; video only");
        return 0;
    }

    const int fsz = g_rec.acodec->frame_size > 0 ? g_rec.acodec->frame_size : 1024;
    g_rec.afifo  = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, 2, 44100);
    g_rec.aframe = av_frame_alloc();
    if (!g_rec.afifo || !g_rec.aframe) {
        OPL_WARN_F("record", "audio: fifo/frame alloc failed; video only");
        return 0;
    }
    g_rec.aframe->nb_samples     = fsz;
    g_rec.aframe->format         = g_rec.acodec->sample_fmt;
    g_rec.aframe->sample_rate    = 44100;
    g_rec.aframe->channels       = 2;
    g_rec.aframe->channel_layout = AV_CH_LAYOUT_STEREO;
    if (av_frame_get_buffer(g_rec.aframe, 0) < 0) {
        OPL_WARN_F("record", "audio: frame buffer alloc failed; video only");
        return 0;
    }

    /* Discard whatever the capture ring accumulated before recording started so
     * audio sample 0 ~aligns with video frame 0. */
    { short scratch[2048 * 2]; while (PsyX_SPUAL_CaptureDrain(scratch, 2048) > 0) {} }
    PsyX_SPUAL_CaptureResetStats();  /* pre-roll backlog is expected; count from here */

    g_rec.a_next_pts   = 0;
    g_rec.audio_enabled = 1;
    OPL_INFO_F("record", "audio: %s 44100 Hz stereo", g_rec.audio_encoder_name);
    return 0;
}

/** @brief Encode one audio frame, or flush the audio encoder, and mux every packet it produces.
 *
 * Reuses the video packet object; the same single-writer rule as
 * rec_encode_one applies.
 * @param af Audio frame, or NULL to flush.
 * @return 0 on success, -1 on a send, receive or write error. */
static int rec_encode_audio_frame(AVFrame* af) {
    int ret = avcodec_send_frame(g_rec.acodec, af);
    if (ret < 0) return -1;
    AVPacket* pkt = g_rec.pkt;  /* reuse the video packet object (same thread) */
    for (;;) {
        ret = avcodec_receive_packet(g_rec.acodec, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) return -1;
        av_packet_rescale_ts(pkt, g_rec.acodec->time_base, g_rec.astream->time_base);
        pkt->stream_index = g_rec.astream->index;
        ret = av_interleaved_write_frame(g_rec.fmt, pkt);
        av_packet_unref(pkt);
        if (ret < 0) return -1;
    }
}

/** @brief Move the captured SPU PCM into the FIFO and encode every whole audio frame it holds.
 *
 * Frame PTS counts samples from 0. An encode error disables audio for the
 * rest of the recording. Runs on the encoder thread, and on the shutdown
 * thread after the join. No effect when audio is disabled. */
static void rec_pump_audio(void) {
    if (!g_rec.audio_enabled) return;
    short buf[4096 * 2];
    int n;
    while ((n = PsyX_SPUAL_CaptureDrain(buf, 4096)) > 0) {
        void* in[1] = { buf };
        if (av_audio_fifo_write(g_rec.afifo, in, n) < 0) break;
        if (n < 4096) break;  /* drained everything available */
    }
    const int fsz = g_rec.aframe->nb_samples;  /* AAC: 1024 */
    short pull[8192 * 2];                       /* >= any sane frame_size */
    while (av_audio_fifo_size(g_rec.afifo) >= fsz) {
        void* out[1] = { pull };
        if (av_audio_fifo_read(g_rec.afifo, out, fsz) < fsz)
            break;
        if (av_frame_make_writable(g_rec.aframe) < 0) break;
        const uint8_t* src[1] = { (const uint8_t*)pull };
        if (swr_convert(g_rec.swr, g_rec.aframe->data, fsz, src, fsz) < 0) break;
        g_rec.aframe->pts = g_rec.a_next_pts;
        g_rec.a_next_pts += fsz;
        if (rec_encode_audio_frame(g_rec.aframe) < 0) { g_rec.audio_enabled = 0; break; }
    }
}

/* How often the encoder thread wakes to drain the SPU capture ring when no
 * video frame is pending. Well under the ring's ~1 s capacity. */
#define REC_AUDIO_PUMP_MS 10

/** @brief Body of the recording encoder thread: encode queued frames and drain the audio ring until the recording stops. */
static int rec_thread_main(void* unused) {
    (void)unused;
    openpete_crash_thread_attach();
    for (;;) {
        SDL_LockMutex(g_rec.mutex);
        /* Timed wait, not an indefinite one: the SPU fills the capture ring at
         * 44.1 kHz of emulated time no matter how slowly video frames arrive,
         * so parking here until the next frame lets the ring overrun and
         * discard audio whenever rendering runs below display rate. Waking
         * regularly keeps the drain independent of the video cadence. */
        while (g_rec.count == 0 && !g_rec.stop) {
            SDL_WaitConditionTimeout(g_rec.cond, g_rec.mutex, REC_AUDIO_PUMP_MS);
            if (g_rec.count == 0 && !g_rec.stop) {
                SDL_UnlockMutex(g_rec.mutex);
                rec_pump_audio();
                SDL_LockMutex(g_rec.mutex);
            }
        }
        if (g_rec.count == 0 && g_rec.stop) {
            SDL_UnlockMutex(g_rec.mutex);
            break;
        }
        const int idx = g_rec.read_idx;
        const uint64_t pts = g_rec.pts_us[idx];
        uint8_t* buf = g_rec.slots[idx];
        SDL_UnlockMutex(g_rec.mutex);

        /* Slot stays counted (producer won't reuse it) until we free it
         * below - so encoding from `buf` outside the lock is safe. */
        if (rec_encode_one(buf, pts) < 0)
            g_rec.failed = 1;

        /* Pump audio at the video-frame cadence (every ~16-33 ms): plenty
         * frequent vs the ~1 s capture ring, and keeps all muxing on this
         * one thread. */
        rec_pump_audio();

        SDL_LockMutex(g_rec.mutex);
        g_rec.read_idx = (idx + 1) % REC_RING_SLOTS;
        g_rec.count--;
        /* Wakes a producer parked on a full ring (CFR backpressure). */
        SDL_BroadcastCondition(g_rec.cond);
        SDL_UnlockMutex(g_rec.mutex);
    }
    return 0;
}

/* ---- lazy encoder build (first captured frame) ---- */

static void rec_teardown(void);  /* fwd */

/** @brief Build the encoder, muxer, scaler, ring and encoder thread on the first captured image.
 *
 * Floors the size to even dimensions, opens the output context, the video
 * encoder (constant frame rate at a multiple of the NTSC tick rate when
 * cfr_mult is set, else microsecond timestamps), the optional audio stream
 * and the file, and writes the container header. Then creates the
 * RGBA-to-encoder-format scaler, allocates the encoder-format frame, the
 * ring slots and the lock, and starts the encoder thread. A failure logs
 * and returns -1 with partial state left for rec_teardown; audio setup
 * failure is not a failure.
 * @param native_w Captured width in pixels.
 * @param native_h Captured height in pixels.
 * @param top_down Nonzero when source rows run top to bottom.
 * @param first_pts_us Timestamp of the first image in microseconds.
 * @return 0 when armed, -1 on failure. */
static int rec_lazy_init(int native_w, int native_h, int top_down, uint64_t first_pts_us) {
    g_rec.native_w = native_w;
    g_rec.native_h = native_h;
    g_rec.src_top_down = top_down;
    g_rec.pbo_depth = GR_PresentPBO_RingDepth();
    if (g_rec.pbo_depth > REC_PBO_PTS_MAX) g_rec.pbo_depth = REC_PBO_PTS_MAX;
    g_rec.pbo_pts_tail = 0;
    g_rec.pbo_pts_count = 0;
    /* yuv420p (and h264 generally) needs even dimensions; ultrawide widening
     * yields odd widths. Floor to even - drops at most one edge row/column. */
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
    if (g_rec.cfr_mult > 0) {
        /* Exact NTSC rational: one tick is 37609/1125000 s (the PSX 240p VBL
         * halved), split into cfr_mult presents. A frame's PTS is its ordinal,
         * so the cadence is perfectly even by construction and the container
         * advertises the true rate (29.913052... / 59.826105... / 119.652210...)
         * rather than a rounded 30 / 60 / 120 - or the 29.97 that belongs to
         * interlaced NTSC and not to anything this game outputs. */
        g_rec.codec->time_base = (AVRational){ (int)OP_VBL_HZ_DEN,
                                               (int)(OP_VBL_HZ_NUM / OP_TICK_VBLS)
                                                   * g_rec.cfr_mult };
        g_rec.codec->framerate = (AVRational){ (int)(OP_VBL_HZ_NUM / OP_TICK_VBLS)
                                                   * g_rec.cfr_mult,
                                               (int)OP_VBL_HZ_DEN };
        g_rec.start_us = 0;   /* PTS is an ordinal, not an offset from t0 */
    } else {
        /* Microsecond timestamps; true VFR (no fixed framerate). */
        g_rec.codec->time_base = (AVRational){ 1, 1000000 };
    }
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

    /* Add the audio stream (if capture is active) BEFORE write_header - all
     * streams must exist first. Non-fatal: falls back to video-only. */
    rec_setup_audio();

    if (!(g_rec.fmt->oformat->flags & AVFMT_NOFILE)) {
        snprintf(g_rec.part_path, sizeof g_rec.part_path, "%s.part", g_rec.path);
        ret = avio_open(&g_rec.fmt->pb, g_rec.part_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            OPL_ERROR_F("record", "avio_open(%s) failed: %s",
                        g_rec.part_path, av_err2str(ret));
            g_rec.part_path[0] = '\0';
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
    OPL_INFO_F("record", "armed: %s [%s %dx%d crf=%d gop=%d %s] %s",
               g_rec.path, g_rec.encoder_name, w, h, g_rec.crf, g_rec.gop,
               g_rec.pixfmt_name,
               g_rec.cfr_mult > 0 ? "cfr" : "vfr");
    if (g_rec.cfr_mult > 0)
        OPL_INFO_F("record", "cfr %.6f fps (%dx the NTSC tick rate), budget %llu frame(s)",
                   OP_TICK_HZ * g_rec.cfr_mult, g_rec.cfr_mult,
                   (unsigned long long)g_rec.max_frames);
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
    g_rec.audio_requested    = !opts->record_no_audio;
    g_rec.audio_encoder_name = opts->record_audio_encoder
                               ? opts->record_audio_encoder : "aac";
    g_rec.max_frames         = (opts->record_frames > 0)
                               ? (uint64_t)opts->record_frames : 0u;
    if (opts->record_cfr > 0.0) {
        g_rec.cfr_mult = (int)(opts->record_cfr / OP_TICK_HZ + 0.5);
        if (g_rec.cfr_mult < 1) g_rec.cfr_mult = 1;
        /* CFR frames are timed in game ticks, not wall-clock seconds, so the
         * capture runs as fast (or as slow) as the machine renders. There is no
         * sound track that could stay in sync with that, so drop audio rather
         * than mux a stream that drifts against the picture. */
        if (g_rec.audio_requested) {
            OPL_INFO_F("record",
                       "--record-cfr: audio capture disabled (CFR video is timed "
                       "in game ticks, not wall-clock seconds)");
            g_rec.audio_requested = 0;
        }
    }
    return 0;
}

/** @brief Claim the ring slot for the next image.
 *
 * Under constant-rate capture the call waits for a free slot, since a
 * missing image would shift the game time of every later one; under
 * variable-rate capture a full ring drops and counts the image, so the game
 * is never stalled. Refuses once the --record-frames budget is filled.
 * @return The slot index to fill, or -1 when the image is not taken. */
static int rec_slot_claim(void) {
    SDL_LockMutex(g_rec.mutex);
    if (g_rec.finished) {
        SDL_UnlockMutex(g_rec.mutex);
        return -1;
    }
    if (g_rec.cfr_mult > 0) {
        while (g_rec.count >= REC_RING_SLOTS && !g_rec.stop)
            SDL_WaitCondition(g_rec.cond, g_rec.mutex);
    } else if (g_rec.count >= REC_RING_SLOTS) {
        SDL_UnlockMutex(g_rec.mutex);
        if (g_rec.dropped == 0)
            OPL_WARN_F("record", "ring full; dropping frame(s) "
                                 "(encoder can't keep up)");
        g_rec.dropped++;
        return -1;
    }
    const int idx = g_rec.write_idx;
    SDL_UnlockMutex(g_rec.mutex);
    return idx;
}

/** @brief Hand a filled ring slot to the encoder thread under its timestamp.
 *
 * Filling the --record-frames budget sets finished; later images are
 * ignored.
 * @param idx Slot returned by rec_slot_claim, already filled.
 * @param pts Capture timestamp in microseconds, or the constant-rate
 *        ordinal. */
static void rec_slot_commit(int idx, uint64_t pts) {
    SDL_LockMutex(g_rec.mutex);
    g_rec.pts_us[idx] = pts;
    g_rec.write_idx   = (idx + 1) % REC_RING_SLOTS;
    g_rec.count++;
    g_rec.captured++;
    if (g_rec.max_frames && g_rec.captured >= g_rec.max_frames)
        g_rec.finished = 1;
    SDL_BroadcastCondition(g_rec.cond);
    SDL_UnlockMutex(g_rec.mutex);
}

/** @brief Return the constant-rate ordinal of a capture timestamp: whole capture periods since the first image, rounded to nearest.
 *
 * One period is 1 / (OP_TICK_HZ * cfr_mult) s, computed in integers from
 * the exact NTSC rational (37609 / 1125000 s per tick), so a present on the
 * capture grid maps to its own ordinal however long the run.
 * @param tag Capture timestamp in microseconds, not before cfr_first_tag.
 * @return The ordinal. */
static uint64_t rec_cfr_ordinal(uint64_t tag) {
    const uint64_t num = (uint64_t)(OP_VBL_HZ_NUM / OP_TICK_VBLS) * (uint64_t)g_rec.cfr_mult;
    const uint64_t den = (uint64_t)OP_VBL_HZ_DEN * 1000000ull;
    return ((tag - g_rec.cfr_first_tag) * num + den / 2) / den;
}

/** @brief Queue one image under constant-rate capture at the ordinal its timestamp names.
 *
 * The first image is ordinal 0. A later image whose ordinal is past the
 * next one expected means presents the pacer owed never reached the
 * encoder: each missing ordinal is filled by repeating the previous image
 * and counted in cfr_filled, so the clip keeps game time and the loss is
 * reported at finalize. An image whose ordinal was already queued is
 * dropped and counted in cfr_dup.
 * @param rgba Image of native_w x native_h in the locked row order.
 * @param tag Capture timestamp in microseconds. */
static void rec_publish_cfr(const uint8_t* rgba, uint64_t tag) {
    const size_t bytes = (size_t)g_rec.native_w * (size_t)g_rec.native_h * 4u;
    if (!g_rec.cfr_started) {
        g_rec.cfr_started   = 1;
        g_rec.cfr_first_tag = tag;
    }
    const uint64_t ord = tag < g_rec.cfr_first_tag ? 0 : rec_cfr_ordinal(tag);
    if (ord < g_rec.cfr_next) {
        g_rec.cfr_dup++;
        return;
    }
    /* Only this thread writes slots, and the encoder only reads them, so
     * the previous image stays intact in its slot while it is copied. */
    while (g_rec.cfr_next < ord) {
        const int idx = rec_slot_claim();
        if (idx < 0) return;
        memcpy(g_rec.slots[idx], g_rec.slots[g_rec.cfr_prev_idx], bytes);
        rec_slot_commit(idx, g_rec.cfr_next++);
        g_rec.cfr_prev_idx = idx;
        g_rec.cfr_filled++;
    }
    /* OPENPETE_REC_LOG: one line per published image, its encoder frame
     * ordinal before the publish and its present tag, which [recq] maps to a
     * VBL. */
    if (getenv("OPENPETE_REC_LOG"))
        fprintf(stderr, "[recpub] n=%llu pts=%llu\n", (unsigned long long)g_rec.captured,
                (unsigned long long)tag);
    const int idx = rec_slot_claim();
    if (idx < 0) return;
    memcpy(g_rec.slots[idx], rgba, bytes);
    rec_slot_commit(idx, g_rec.cfr_next++);
    g_rec.cfr_prev_idx = idx;
}

/** @brief Copy one RGBA image into the encoder slot ring and wake the encoder thread.
 *
 * Constant-rate capture goes through rec_publish_cfr, which replaces
 * @p pts_us with the ordinal it names. Variable-rate capture keeps
 * @p pts_us and drops the image when the ring is full.
 * @param rgba Image of native_w x native_h in the locked row order.
 * @param pts_us Capture timestamp in microseconds. */
static void rec_publish(const uint8_t* rgba, uint64_t pts_us) {
    if (g_rec.cfr_mult > 0) {
        rec_publish_cfr(rgba, pts_us);
        return;
    }
    const int idx = rec_slot_claim();
    if (idx < 0) return;
    memcpy(g_rec.slots[idx], rgba,
           (size_t)g_rec.native_w * (size_t)g_rec.native_h * 4u);
    rec_slot_commit(idx, pts_us);
}

/** @brief Map the oldest in-flight PBO read and publish it with its submit-time timestamp.
 *
 * A mapped image whose size differs from the locked size is discarded.
 * The timestamp is consumed even when the map fails. No effect when no
 * read is in flight. */
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
        if (rec_lazy_init(w, h, /*top_down=*/0, pts_us) != 0) {  /* PsyCross FBO is bottom-up */
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
         * consistent. */
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

int openpete_record_supported(void) { return 1; }

int openpete_record_active(void) {
    return g_rec.enabled && !g_rec.failed && !g_rec.finished;
}

int openpete_record_finished(void) {
    return g_rec.enabled && g_rec.finished;
}

/* Native-renderer submit: a synchronous copy into the encoder ring. */
void openpete_record_submit_frame(const uint8_t* rgba, int w, int h,
                                  int top_down, uint64_t pts_us) {
    if (!g_rec.enabled || g_rec.failed || !rgba || w <= 0 || h <= 0)
        return;
    if (!g_rec.initialized) {
        if (rec_lazy_init(w, h, top_down, pts_us) != 0) {
            OPL_ERROR_F("record", "init failed; recording disabled");
            g_rec.failed = 1;
            rec_teardown();
            return;
        }
    }
    if (w != g_rec.native_w || h != g_rec.native_h) {
        if (!g_rec.warned_resize) {
            OPL_WARN_F("record",
                       "native size changed %dx%d -> %dx%d mid-record; dropping",
                       g_rec.native_w, g_rec.native_h, w, h);
            g_rec.warned_resize = 1;
        }
        g_rec.dropped++;
        return;
    }
    rec_publish(rgba, pts_us);
}

/** @brief Free every encoder, stream, ring and lock, and log the capture ring drop count.
 *
 * A non-zero drop count is a warning, because dropped capture frames
 * shorten the audio track against emulated time. Freed pointers are
 * cleared, so a second call frees nothing. The encoder thread must be
 * joined or never started. */
static void rec_teardown(void) {
    /* The SPU capture ring discards its oldest frames when the encoder falls
     * behind, which shortens the audio track relative to emulated time. Report
     * it: a silently time-compressed recording is useless for timing or level
     * work and looks identical to a good one. */
    {
        unsigned long long produced = 0, dropped = 0;
        PsyX_SPUAL_CaptureStats(&produced, &dropped);
        if (dropped) {
            OPL_WARN_F("record",
                       "audio capture dropped %llu of %llu pushed frames (%.2f%%) - "
                       "recorded audio is TIME-COMPRESSED, do not use it for "
                       "timing or level comparison",
                       dropped, produced,
                       100.0 * (double)dropped / (double)produced);
        } else if (produced) {
            OPL_INFO_F("record", "audio capture: %llu frames, 0 dropped", produced);
        }
    }
    if (g_rec.afifo) { av_audio_fifo_free(g_rec.afifo); g_rec.afifo = NULL; }
    if (g_rec.aframe){ av_frame_free(&g_rec.aframe); }
    if (g_rec.swr)   { swr_free(&g_rec.swr); }
    if (g_rec.acodec){ avcodec_free_context(&g_rec.acodec); }
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

/** @brief Rename the closed `<path>.part` stream over path, or leave it in place after a failure.
 *
 * Runs after rec_teardown closed the stream. The rename is the unsynced
 * capture class (op_atomic_rename), so a previous file at path stays whole
 * until a finished recording replaces it. A stream whose encoder setup
 * failed holds no picture and is removed; after a later encoder or trailer
 * failure the `.part` file is kept and named in the log. */
static void rec_publish_part(void) {
    if (!g_rec.part_path[0]) return;
    if (!g_rec.initialized) {
        (void)remove(g_rec.part_path);
        return;
    }
    if (g_rec.failed) {
        OPL_WARN_F("record", "recording incomplete; %s left in place, %s not replaced",
                   g_rec.part_path, g_rec.path);
        return;
    }
    if (op_atomic_rename(g_rec.part_path, g_rec.path, OP_ATOMIC_NOSYNC) == 0)
        OPL_INFO_F("record", "renamed %s to %s", g_rec.part_path, g_rec.path);
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
        SDL_BroadcastCondition(g_rec.cond);
        SDL_UnlockMutex(g_rec.mutex);
        SDL_WaitThread(g_rec.thread, NULL);
        g_rec.thread = NULL;

        /* Flush video + audio encoders and finalize, unless an earlier
         * error poisoned the stream. Thread is joined, so this thread is the
         * sole writer. */
        if (!g_rec.failed) {
            rec_encode_one(NULL, 0);
            if (g_rec.audio_enabled) {
                rec_pump_audio();                  /* encode any whole frames left */
                rec_encode_audio_frame(NULL);      /* flush the AAC encoder */
            }
            int ret = av_write_trailer(g_rec.fmt);
            if (ret < 0) {
                OPL_ERROR_F("record", "av_write_trailer failed: %s",
                            av_err2str(ret));
                g_rec.failed = 1;
            }
        }
        OPL_INFO_F("record", "finalized %s: %llu frame(s) captured, "
                             "%llu dropped, audio=%s",
                   g_rec.path,
                   (unsigned long long)g_rec.captured,
                   (unsigned long long)g_rec.dropped,
                   g_rec.audio_enabled ? "yes" : "no");
        if (g_rec.cfr_filled || g_rec.cfr_dup)
            OPL_WARN_F("record",
                       "cfr: %llu owed present(s) never reached the encoder and "
                       "were filled by repeating the previous frame; %llu "
                       "frame(s) dropped for an ordinal already written",
                       (unsigned long long)g_rec.cfr_filled,
                       (unsigned long long)g_rec.cfr_dup);
    } else if (!g_rec.failed) {
        OPL_WARN_F("record", "no frames captured (present FBO never ready?); "
                             "no file written");
    }

    rec_teardown();
    rec_publish_part();
    memset(&g_rec, 0, sizeof(g_rec));
}

#else /* !OPENPETE_HAS_LIBAV */

// libav unavailable for this target (cross-compile / OPENPETE_ENABLE_RECORD
// off). Provide stubs so the runtime links and --record degrades gracefully:
// it warns once at init and otherwise does nothing.

int openpete_record_init(const openpete_options_t* opts)
{
    if (opts && opts->record_path) {
        OPL_WARN_F("record", "--record %s ignored: this build has no video "
                             "encoder (libav* not available for this target)",
                   opts->record_path);
    }
    return 0;  /* not a fatal setup error - just unavailable */
}

void openpete_record_capture_frame(uint64_t pts_us) { (void)pts_us; }

int  openpete_record_supported(void) { return 0; }

int  openpete_record_active(void) { return 0; }

int  openpete_record_finished(void) { return 0; }

void openpete_record_submit_frame(const uint8_t* rgba, int w, int h,
                                  int top_down, uint64_t pts_us) {
    (void)rgba; (void)w; (void)h; (void)top_down; (void)pts_us;
}

void openpete_record_shutdown(void) {}

#endif /* OPENPETE_HAS_LIBAV */
