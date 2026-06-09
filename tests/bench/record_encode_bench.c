// tests/bench/record_encode_bench.c — encode-throughput validation for the
// --record pipeline (plan openpete-record-video.md Phase 7).
//
// Drives the SAME encode path record.c uses (swscale RGBA -> yuv420p ->
// libavcodec encoder, microsecond PTS, interleaved mux) with synthetic
// frames fed as fast as the encoder will take them, and reports the
// sustained encode rate. This isolates "can the encoder keep up?" from the
// game loop: the headless game only produces ~140 present-fps, so it can't
// exercise the 480-fps target by itself.
//
// Synthetic frames carry spatial gradients + a moving block (not flat color,
// which would compress trivially and overstate fps; not noise, which is
// worst-case incompressible and understates it) — a rough stand-in for game
// content's compressibility.
//
// Usage:
//   record_encode_bench [--width W] [--height H] [--frames N]
//                       [--fps F] [--encoder NAME] [--crf N]
//                       [--out PATH] [--min-fps M]
// Prints a "RESULT ..." line; exits 0 if sustained fps >= --min-fps (default
// 1, i.e. informational), 1 on encode error or below the floor.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fill_synthetic(uint8_t* rgba, int w, int h, int frame) {
    /* Gradients + a moving block: spatial detail and temporal change so the
     * encoder does representative work. */
    const int bx = (frame * 7) % (w > 64 ? w - 64 : 1);
    const int by = (frame * 5) % (h > 64 ? h - 64 : 1);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t* p = row + x * 4;
            int in_block = (x >= bx && x < bx + 64 && y >= by && y < by + 64);
            p[0] = (uint8_t)(x + frame);
            p[1] = (uint8_t)(y + frame);
            p[2] = (uint8_t)((x ^ y) + frame * 3);
            p[3] = 0xFF;
            if (in_block) { p[0] = 0xFF; p[1] = 0x20; p[2] = 0x20; }
        }
    }
}

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);  /* quiet per-frame x264 stats */

    int w = 854, h = 480, frames = 1000, fps = 480, crf = 23;
    double min_fps = 1.0;
    const char* encoder = "libx264";
    const char* out = "/tmp/openpete-encode-bench.mkv";

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (fprintf(stderr, "missing value for %s\n", a), exit(2), ""))
        if      (!strcmp(a, "--width"))   w = atoi(NEXT());
        else if (!strcmp(a, "--height"))  h = atoi(NEXT());
        else if (!strcmp(a, "--frames"))  frames = atoi(NEXT());
        else if (!strcmp(a, "--fps"))     fps = atoi(NEXT());
        else if (!strcmp(a, "--crf"))     crf = atoi(NEXT());
        else if (!strcmp(a, "--encoder")) encoder = NEXT();
        else if (!strcmp(a, "--out"))     out = NEXT();
        else if (!strcmp(a, "--min-fps")) min_fps = atof(NEXT());
        else { fprintf(stderr, "unknown arg: %s\n", a); return 2; }
        #undef NEXT
    }
    w &= ~1; h &= ~1;
    if (w <= 0 || h <= 0 || frames <= 0 || fps <= 0) {
        fprintf(stderr, "bad dimensions/frames/fps\n");
        return 2;
    }

    AVFormatContext* fmt = NULL;
    if (avformat_alloc_output_context2(&fmt, NULL, NULL, out) < 0 || !fmt) {
        fprintf(stderr, "alloc_output_context2 failed\n"); return 1;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) { fprintf(stderr, "unknown encoder '%s'\n", encoder); return 1; }
    AVStream* st = avformat_new_stream(fmt, NULL);
    AVCodecContext* cc = avcodec_alloc_context3(codec);
    if (!st || !cc) { fprintf(stderr, "stream/ctx alloc failed\n"); return 1; }
    cc->codec_id  = codec->id;
    cc->width     = w;
    cc->height    = h;
    cc->pix_fmt   = AV_PIX_FMT_YUV420P;
    cc->gop_size  = 120;
    cc->time_base = (AVRational){ 1, 1000000 };
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (cc->priv_data) {
        char crf_s[16]; snprintf(crf_s, sizeof(crf_s), "%d", crf);
        av_opt_set(cc->priv_data, "preset", "veryfast", 0);
        av_opt_set(cc->priv_data, "crf", crf_s, 0);
    }
    if (avcodec_open2(cc, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2(%s) failed\n", encoder); return 1;
    }
    avcodec_parameters_from_context(st->codecpar, cc);
    st->time_base = cc->time_base;
    if (!(fmt->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&fmt->pb, out, AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "avio_open(%s) failed\n", out); return 1;
    }
    if (avformat_write_header(fmt, NULL) < 0) {
        fprintf(stderr, "write_header failed\n"); return 1;
    }

    struct SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_RGBA,
                                            w, h, AV_PIX_FMT_YUV420P,
                                            SWS_POINT, NULL, NULL, NULL);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt  = av_packet_alloc();
    frame->format = AV_PIX_FMT_YUV420P; frame->width = w; frame->height = h;
    if (!sws || !frame || !pkt || av_frame_get_buffer(frame, 0) < 0) {
        fprintf(stderr, "sws/frame/pkt setup failed\n"); return 1;
    }
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!rgba) { fprintf(stderr, "rgba alloc failed\n"); return 1; }

    const int64_t pts_step = 1000000 / fps;   /* microseconds between frames */
    const double t0 = now_sec();
    int rc = 0;
    for (int i = 0; i < frames && rc == 0; ++i) {
        fill_synthetic(rgba, w, h, i);
        if (av_frame_make_writable(frame) < 0) { rc = 1; break; }
        const uint8_t* src[4] = { rgba, NULL, NULL, NULL };
        const int stride[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, src, stride, 0, h, frame->data, frame->linesize);
        frame->pts = (int64_t)i * pts_step;
        if (avcodec_send_frame(cc, frame) < 0) { rc = 1; break; }
        while (1) {
            int r = avcodec_receive_packet(cc, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) { rc = 1; break; }
            av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
    }
    const double encode_secs = now_sec() - t0;

    /* Flush. */
    if (rc == 0) {
        avcodec_send_frame(cc, NULL);
        while (1) {
            int r = avcodec_receive_packet(cc, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) break;
            av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
        av_write_trailer(fmt);
    }

    const double sustained = encode_secs > 0 ? frames / encode_secs : 0.0;
    printf("RESULT encoder=%s %dx%d frames=%d wall=%.3fs fps=%.1f target=480 (min=%.0f)\n",
           encoder, w, h, frames, encode_secs, sustained, min_fps);

    free(rgba);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt->pb);
    avformat_free_context(fmt);

    if (rc != 0) { fprintf(stderr, "encode error\n"); return 1; }
    if (sustained < min_fps) {
        fprintf(stderr, "sustained %.1f fps below floor %.0f\n", sustained, min_fps);
        return 1;
    }
    return 0;
}
