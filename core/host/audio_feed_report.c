// core/host/audio_feed_report.c - the audio device feed's counters and
// event ring as text.
//
// PsyCross's device feed (third_party/PsyCross/src/audio/PsyX_SPUFeed.cpp)
// counts its frames and records every timeline event (primes, underruns,
// drops, stretch edges, slips, holds, phase changes, a sample every 250 ms
// of device time) into a ring of the last PSYX_SPUFEED_EVENTS events. A
// snapshot copies both on the calling thread; the exit summary logs its
// counter lines and the bug report writes it whole as audio-feed.txt.
#include "openpete_audio_feed_report.h"

#include "openpete.h"
#include "openpete_fiber.h"
#include "openpete_interp.h"

/* PsyCross's private header, not on any public include path. */
#include "../../third_party/PsyCross/src/audio/PsyX_SPUFeed.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void PsyX_SPUAL_FeatureStats(unsigned long long* reverb_cycles, unsigned long long* reverb_in,
                                    unsigned long long* noise, unsigned long long* pmon,
                                    unsigned long long* sweep);
extern void PsyX_SPUAL_HostStats(unsigned long long* plays, unsigned long long* clip_frames,
                                 unsigned long long* stream_frames);

/* audio-feed.txt's layout version. */
#define AFR_VERSION 1

struct openpete_audio_feed_snap {
    int                 have_counters;
    PsyX_SPUFeed_Stats  st;
    unsigned long long  spu[8];      /* reverb_cycles, reverb_in, noise, pmon, sweep, plays, clip, stream */
    int                 mute;
    uint64_t            wall_ns;     /* SDL_GetTicksNS() at the snapshot: the events' clock */
    int64_t             present_ns;  /* the present clock at the snapshot */
    int                 vbl;         /* the device VBL count at the snapshot */
    PsyX_SPUFeed_Event* ev;          /* oldest first */
    int                 ev_n;
    unsigned long long  ev_total, ev_skipped;
};

openpete_audio_feed_snap_t* openpete_audio_feed_snapshot(int counters, int events)
{
    openpete_audio_feed_snap_t* s = (openpete_audio_feed_snap_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->wall_ns = SDL_GetTicksNS();
    s->present_ns = (int64_t)openpete_interp_present_now();
    s->vbl = SDL_GetAtomicInt(&openpete_vbl_device);
    s->mute = openpete_get_mute();
    if (counters) {
        PsyX_SPUFeed_GetStats(&s->st);
        PsyX_SPUAL_FeatureStats(&s->spu[0], &s->spu[1], &s->spu[2], &s->spu[3], &s->spu[4]);
        PsyX_SPUAL_HostStats(&s->spu[5], &s->spu[6], &s->spu[7]);
        s->have_counters = 1;
    }
    if (events) {
        s->ev = (PsyX_SPUFeed_Event*)malloc(PSYX_SPUFEED_EVENTS * sizeof *s->ev);
        if (s->ev)
            s->ev_n = PsyX_SPUFeed_GetEvents(s->ev, PSYX_SPUFEED_EVENTS, &s->ev_total, &s->ev_skipped);
    }
    return s;
}

void openpete_audio_feed_snap_free(openpete_audio_feed_snap_t* s)
{
    if (!s) return;
    free(s->ev);
    free(s);
}

int openpete_audio_feed_snap_events(const openpete_audio_feed_snap_t* s)
{
    return s ? s->ev_n : 0;
}

/** @brief Format one line and pass it to the sink; a line longer than 1023 bytes is truncated.
 * @param line The sink.
 * @param ud Passed to @p line.
 * @param fmt printf format. */
static void emitf(openpete_audio_feed_line_fn line, void* ud, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    line(ud, buf);
}

/** @brief The name of a feed device kind (PSYX_SPUFEED_DEV_*).
 * @param d The kind.
 * @return "openal", "synthetic" or "none". */
static const char* device_name(int d)
{
    return d == PSYX_SPUFEED_DEV_OPENAL ? "openal" : d == PSYX_SPUFEED_DEV_SYNTHETIC ? "synthetic" : "none";
}

/** @brief Emit the feed's state at the snapshot: device, hold, playing, phase, depth, stretch, nominal rate and slips.
 * @param st The counters.
 * @param line The sink.
 * @param ud Passed to @p line. */
static void emit_now(const PsyX_SPUFeed_Stats* st, openpete_audio_feed_line_fn line, void* ud)
{
    const int ph = st->cur_phase >= 0 && st->cur_phase < PSYX_SPUFEED_PHASES ? st->cur_phase : 0;
    emitf(line, ud, "feed: now: device=%s hold=%d playing=%d phase=%s tag=%d queued=%d fifo=%u depth_s=%.0f "
          "stretch_on=%d st_rate=%.4f nominal=%.4f slips=%llu behind=%.3f events=%llu",
          device_name(st->device), st->hold, st->playing, st->phase_name[ph][0] ? st->phase_name[ph] : "?",
          st->tag, st->queued, st->fifo, st->depth_s, st->st_on, st->st_rate, st->nominal, st->slips,
          st->behind, st->events);
}

/** @brief Emit the feed's frame counters, accounting, pitch, drift, stretch and output-hash lines.
 * @param st The counters.
 * @param line The sink.
 * @param ud Passed to @p line. */
static void emit_feed(const PsyX_SPUFeed_Stats* st, openpete_audio_feed_line_fn line, void* ud)
{
    emitf(line, ud, "feed: pushed=%llu held=%llu overflow=%llu consumed=%llu periods=%llu "
          "underruns=%llu primes=%llu target=%d depth_min=%d p01=%d p50=%d p99=%d ratio=%.5f rate=%.5f",
          st->pushed, st->held, st->overflow, st->consumed, st->pulls, st->underruns, st->primes,
          st->target, st->depth_min, st->depth_p01, st->depth_p50, st->depth_p99, st->ratio, st->rate);
    emitf(line, ud, "feed: accounting: pushed=%llu = taken=%llu + overflow=%llu + discarded=%llu "
          "+ fifo=%u (residual %lld); consumed=%llu = data_out=%llu + silence_out=%llu",
          st->pushed, st->taken, st->overflow, st->discarded, st->fifo,
          (long long)(st->pushed - st->taken - st->overflow - st->discarded - st->fifo),
          st->consumed, st->data_out, st->silence_out);
    emitf(line, ud, "feed: pitch: pumps=%llu median=%+.2fc p99=%.2fc max=%+.2fc",
          st->pitch_n, st->pitch_med_c, st->pitch_p99_c, st->pitch_max_c);
    emitf(line, ud, "feed: drift: drift=%.6f (%+.1f ppm) speed=%.4f fits=%llu refits=%llu "
          "behind_used=%.1f behind_dropped=%.1f",
          st->drift, (1.0 / st->drift - 1.0) * 1e6, st->speed, st->drift_fits, st->drift_refits,
          st->slips_used, st->slips_dropped);
    emitf(line, ud, "feed: stretch: hops=%llu engages=%llu on=%d corr_p01=%.2f corr_p50=%.2f lags=%llu "
          "tempo_lo=%.5f tempo_hi=%.5f",
          st->st_hops, st->st_engages, st->st_on, st->st_corr_p01, st->st_corr_p50, st->lags,
          st->tempo_lo, st->tempo_hi);
    emitf(line, ud, "feed: out_hash=%016llx data_out=%llu tap_frames=%llu", st->out_hash, st->data_out,
          st->tap_frames);
    for (int i = 0; i < PSYX_SPUFEED_PHASES; i++) {
        const PsyX_SPUFeed_PhaseStats* p = &st->phase[i];
        if (!p->pushed && !p->held) continue;
        emitf(line, ud, "feed: phase %s: pushed=%llu held=%llu overflow=%llu underruns=%llu primes=%llu",
              st->phase_name[i][0] ? st->phase_name[i] : "?", p->pushed, p->held, p->overflow,
              p->underruns, p->primes);
    }
}

void openpete_audio_feed_counters(const openpete_audio_feed_snap_t* s, openpete_audio_feed_line_fn line, void* ud)
{
    if (!s || !s->have_counters) return;
    const unsigned long long* u = s->spu;
    emitf(line, ud, "spu: reverb_cycles=%llu reverb_in=%llu noise=%llu pmon=%llu sweep=%llu "
          "host_plays=%llu host_clip_frames=%llu host_stream_frames=%llu",
          u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
    if (!s->st.pulls && !s->st.pushed) {
        emitf(line, ud, "feed: no frame pushed or consumed (device %s)", device_name(s->st.device));
        return;
    }
    emit_feed(&s->st, line, ud);
    emit_now(&s->st, line, ud);
}

/** @brief The phase name a snapshot's counters give an index, or the index as text when the counters were not read.
 * @param s The snapshot.
 * @param ph The phase index.
 * @param buf Scratch for the index form.
 * @param cap Its size.
 * @return The name. */
static const char* phase_name(const openpete_audio_feed_snap_t* s, int ph, char* buf, size_t cap)
{
    if (s->have_counters && ph >= 0 && ph < PSYX_SPUFEED_PHASES && s->st.phase_name[ph][0])
        return s->st.phase_name[ph];
    snprintf(buf, cap, "%d", ph);
    return buf;
}

/** @brief Emit audio-feed.txt's header: version, mute, device, snapshot clocks, counter and event state, and the note a reader needs when no audio was heard or fed.
 * @param s The snapshot.
 * @param line The sink.
 * @param ud Passed to @p line. */
static void emit_header(const openpete_audio_feed_snap_t* s, openpete_audio_feed_line_fn line, void* ud)
{
    emitf(line, ud, "# audio-feed.txt: the audio device feed at the snapshot; the layout is in the developer "
          "page on bug reports");
    emitf(line, ud, "version=%d", AFR_VERSION);
    emitf(line, ud, "mute=%d", s->mute);
    emitf(line, ud, "device=%s", s->have_counters ? device_name(s->st.device) : "unknown");
    emitf(line, ud, "snapshot_wall_ns=%llu", (unsigned long long)s->wall_ns);
    emitf(line, ud, "snapshot_present_ns=%lld", (long long)s->present_ns);
    emitf(line, ud, "snapshot_vbl=%d", s->vbl);
    emitf(line, ud, "counters=%s", s->have_counters ? "read" : "not read (fatal exit off the sim's thread)");
    emitf(line, ud, "ring=%d", PSYX_SPUFEED_EVENTS);
    emitf(line, ud, "events_total=%llu", s->ev_total);
    emitf(line, ud, "events_held=%d", s->ev_n);
    emitf(line, ud, "events_skipped=%llu", s->ev_skipped);
    if (s->mute)
        emitf(line, ud, "note=muted: the SPU output was silenced (--mute or the overlay); the feed carries that "
              "silence and counts and records it as it would sound");
    if (s->have_counters && s->st.device == PSYX_SPUFEED_DEV_NONE)
        emitf(line, ud, "note=no audio device opened: the feed discarded every push; only phase events are "
              "recorded");
}

/** @brief Emit one event row: seconds before the snapshot, VBL tag, device frames consumed, name, phase, hold/playing/stretch flags, then the controller's values.
 * @param s The snapshot (its clock and phase names).
 * @param e The event.
 * @param line The sink.
 * @param ud Passed to @p line. */
static void emit_event(const openpete_audio_feed_snap_t* s, const PsyX_SPUFeed_Event* e,
                       openpete_audio_feed_line_fn line, void* ud)
{
    char pb[16];
    const char flags[4] = {
        (e->flags & PSYX_SPUFEED_EVF_HOLD) ? 'h' : '-', (e->flags & PSYX_SPUFEED_EVF_PLAYING) ? 'p' : '-',
        (e->flags & PSYX_SPUFEED_EVF_STRETCH) ? 's' : '-', '\0'};
    emitf(line, ud, "%.3f %d %llu %s %s %s %.6f %.4f %.6f %.0f %.4f %u %u %llu %.3f %llu %.1f",
          ((double)e->wall_ns - (double)s->wall_ns) / 1e9, e->tag, e->consumed, PsyX_SPUFeed_EventName(e->kind),
          phase_name(s, e->phase, pb, sizeof pb), flags, e->ratio, e->speed, e->drift, e->depth_s, e->st_rate,
          e->fifo, e->underruns, e->slips, e->behind, e->pushed, e->value);
}

void openpete_audio_feed_text(const openpete_audio_feed_snap_t* s, openpete_audio_feed_line_fn line, void* ud)
{
    if (!s) return;
    emit_header(s, line, ud);
    openpete_audio_feed_counters(s, line, ud);
    emitf(line, ud, "# events, oldest first: t_s vbl consumed event phase flags ratio speed drift depth_s st_rate "
          "fifo underruns slips behind pushed value");
    for (int i = 0; i < s->ev_n; i++)
        emit_event(s, &s->ev[i], line, ud);
}
