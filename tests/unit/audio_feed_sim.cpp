/* tests/unit/audio_feed_sim.cpp - drives the audio device feed on its
 * synthetic device without a game process and reports the queued latency
 * after a stretch in which the producer ran slower than real time.
 *
 * PsyX_SPUFeed.cpp is compiled into this TU so the driver reaches the
 * FIFO; its OpenAL calls link but never run (the synthetic device replaces
 * the source).
 *
 * Clock: the driver's own scheduled clock in ns, starting at 1 s, playing
 * the part of the game's virtual clock. Producer: one push per guest VBL
 * (44100 / (2250000/37609) frames, the fraction carried), a stereo sine
 * (L 440 Hz, R 660 Hz, amplitude 12000), on a grid of one VBL period. Each
 * push lands at the later of its grid instant and the previous push, plus
 * the producer model's delay (PsyX_SPUFeed_SimProducerDelay: 0-1 ms jitter,
 * a 45 ms stall every 1.5-2.5 s). Before each push the device and the
 * consumer's wakes run to its instant through PsyX_SPUFeed_SimRunTo, the
 * call the game's consumer thread makes inside each virtual-clock step.
 *
 * Input: OPENPETE_AUDIO_FEED_SIM=DRIFT[,SEED[,STALL[,SPEED@START+DUR]]].
 * The fourth field is the producer-speed span: from START to START+DUR
 * seconds of producer time (VBLs pushed over the VBL rate) the producer
 * pushes SPEED of real time (each VBL's grid step is 1/SPEED of a VBL
 * period) while its nominal rate stays 1, as when the host's slip clamp
 * discards wall time the guest could not keep up with.
 *
 * Instrument: the queued latency after every push, device frames plus the
 * FIFO at the resampling ratio (output frames, reported in ms); its maximum
 * from SETTLE seconds after the span ends to the end of the run; and at
 * every device run before a push whose discard count rose, the largest
 * frame-to-frame step of the FIFO sequence the resampler reads across the
 * splice (the frame before the old tail, then the 2048 frames from the new
 * tail), against the sine's own largest step.
 *
 * Pitch (reported, no criterion): the resampling ratio against the
 * nominal rate in cents, 1200 log2(ratio / nominal), at every push: its
 * extreme inside the span and from SETTLE seconds after it, and the feed's
 * own whole-run pitch statistics.
 *
 * Pass: that maximum at or below the bound target + deadband + 3440 frames
 * (one stall's catch-up burst).
 * Engagement: VBLs pushed inside the span and latency samples in the
 * measured window; either at zero is a refusal.
 *
 * Usage: audio_feed_sim [--seconds=S] [--settle=S] [--trace=PATH]
 * Exit 0 PASS, 2 FAIL, 3 refusal (zero engagement), 64 usage. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"     // warning-ratchet: PsyCross source compiled in unchanged
#include "../../third_party/PsyCross/src/audio/PsyX_SPUFeed.cpp"
#pragma GCC diagnostic pop

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_VBL_HZ      (2250000.0 / 37609.0)
#define SIM_BURST       3440
#define SIM_AMP         12000.0
#define SIM_SPLICE_SPAN 2048

namespace {

struct Span {
	double speed = 1.0, start = 0.0, dur = 0.0;
};

/** @brief Parse OPENPETE_AUDIO_FEED_SIM into the device drift, seed, start stall and producer-speed span.
 * @param spec The variable's value.
 * @param drift Receives the device drift.
 * @param seed Receives the generator seed.
 * @param stall Receives the start stall, s.
 * @param span Receives the producer-speed span (speed 1 when absent).
 * @return 1 when the spec parses, 0 otherwise. */
int parse_spec(const char* spec, double* drift, unsigned* seed, double* stall, Span* span)
{
	char* end = nullptr;
	*drift = strtod(spec, &end);
	if (end == spec) return 0;
	*seed = 1u;
	*stall = 0.0;
	if (*end == ',') *seed = (unsigned)strtoul(end + 1, &end, 10);
	if (*end == ',') *stall = strtod(end + 1, &end);
	if (*end == ',')
	{
		span->speed = strtod(end + 1, &end);
		if (*end != '@') return 0;
		span->start = strtod(end + 1, &end);
		if (*end != '+') return 0;
		span->dur = strtod(end + 1, &end);
		if (span->speed <= 0.0 || span->dur < 0.0) return 0;
	}
	return *end == '\0';
}

/** @brief The largest step between consecutive frames of the sequence the resampler reads after a splice: `prev`, then SIM_SPLICE_SPAN FIFO frames from `tail`.
 * @param prev The frame before the old tail (L, R).
 * @param tail The FIFO index of the new tail.
 * @param avail Frames readable from `tail`.
 * @return The largest absolute step over both channels. */
int splice_step(const int16_t prev[2], uint32_t tail, uint32_t avail)
{
	int worst = 0;
	const uint32_t n = avail < SIM_SPLICE_SPAN ? avail : SIM_SPLICE_SPAN;
	int last[2] = { prev[0], prev[1] };
	for (uint32_t i = 0; i < n; i++)
	{
		const uint32_t k = (tail + i) % FEED_FIFO_FRAMES;
		for (int c = 0; c < 2; c++)
		{
			const int v = g_feed.fifo[k * 2 + c];
			const int d = abs(v - last[c]);
			if (d > worst) worst = d;
			last[c] = v;
		}
	}
	return worst;
}

/* One run: its options, the producer's state and the meters. */
struct Run {
	double seconds = 250.0, settle = 2.0;
	const char* trace_path = nullptr;
	double drift = 0.0, stall = 0.0;
	unsigned seed = 1u;
	Span span;
	FILE* trace = nullptr;
	double carry = 0.0, next_trace = 0.0;
	uint64_t frame = 0, span_vbls = 0, samples = 0, splices = 0;
	double lat_max = 0.0, lat_max_t = 0.0, peak_any = 0.0, peak_any_t = 0.0;
	int splice_worst = 0;
	double cents_span = 0.0, cents_after = 0.0;   /* signed extremes of the pitch deviation */
};

/** @brief Parse the command line and OPENPETE_AUDIO_FEED_SIM into `r`.
 * @param r The run.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 1 when both parse, 0 after printing the usage error. */
int parse_run(Run* r, int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if (!strncmp(a, "--seconds=", 10)) r->seconds = atof(a + 10);
		else if (!strncmp(a, "--settle=", 9)) r->settle = atof(a + 9);
		else if (!strncmp(a, "--trace=", 8)) r->trace_path = a + 8;
		else { fprintf(stderr, "audio_feed_sim: unknown argument %s\n", a); return 0; }
	}
	const char* spec = getenv("OPENPETE_AUDIO_FEED_SIM");
	if (spec && parse_spec(spec, &r->drift, &r->seed, &r->stall, &r->span))
		return 1;
	fprintf(stderr, "audio_feed_sim: OPENPETE_AUDIO_FEED_SIM=DRIFT[,SEED[,STALL[,SPEED@START+DUR]]] required\n");
	return 0;
}

/** @brief Push one VBL of the sine, its frames slowed to the span's speed inside the span.
 * @param r The run.
 * @param in_span Nonzero inside the producer-speed span. */
void push_vbl(Run* r, int in_span)
{
	static int16_t buf[2048 * 2];
	const double w0 = 2.0 * M_PI * 440.0 / FEED_RATE, w1 = 2.0 * M_PI * 660.0 / FEED_RATE;
	r->carry += FEED_RATE / SIM_VBL_HZ;
	const int n = (int)r->carry;
	r->carry -= n;
	for (int i = 0; i < n; i++, r->frame++)
	{
		buf[i * 2 + 0] = (int16_t)lrint(SIM_AMP * sin(w0 * (double)r->frame));
		buf[i * 2 + 1] = (int16_t)lrint(SIM_AMP * sin(w1 * (double)r->frame));
	}
	if (in_span)
		r->span_vbls++;
	PsyX_SPUFeed_Push(buf, n);
}

/** @brief Run the device and the consumer to `to_ns` and measure the splice of any frames the consumer dropped.
 * @param r The run.
 * @param t Producer time, s.
 * @param to_ns The clock instant, ns. */
void run_device(Run* r, double t, long long to_ns)
{
	const uint32_t tail0 = g_feed.tail;
	const uint64_t disc0 = g_feed.discarded;
	const int16_t prev0[2] = { g_feed.prev[0], g_feed.prev[1] };
	PsyX_SPUFeed_SimRunTo(to_ns);
	if (g_feed.discarded == disc0) return;
	const uint32_t d = (uint32_t)(g_feed.discarded - disc0);
	const uint32_t newtail = (tail0 + d) % FEED_FIFO_FRAMES;
	const uint32_t avail = (g_feed.head + FEED_FIFO_FRAMES - newtail) % FEED_FIFO_FRAMES;
	const int s = splice_step(prev0, newtail, avail);
	if (s > r->splice_worst) r->splice_worst = s;
	r->splices++;
	printf("splice t=%.3f dropped=%u step=%d\n", t, d, s);
}

/** @brief Keep `v` in `*extreme` when it is farther from 0.
 * @param extreme The signed extreme so far.
 * @param v The new value. */
void keep_extreme(double* extreme, double v)
{
	if (fabs(v) > fabs(*extreme)) *extreme = v;
}

/** @brief Sample the queued latency and the pitch deviation after a push, and write the trace line every 250 ms.
 * @param r The run.
 * @param t Producer time, s.
 * @param in_span Nonzero inside the producer-speed span. */
void sample(Run* r, double t, int in_span)
{
	const double lat = device_queued() + g_feed.count / g_feed.ratio;
	if (lat > r->peak_any) { r->peak_any = lat; r->peak_any_t = t; }
	const double cents = 1200.0 * log2(g_feed.ratio / g_feed.nominal);
	if (g_feed.playing && in_span) keep_extreme(&r->cents_span, cents);
	if (r->span.dur > 0.0 && t >= r->span.start + r->span.dur + r->settle)
	{
		if (g_feed.playing) keep_extreme(&r->cents_after, cents);
		r->samples++;
		if (lat > r->lat_max) { r->lat_max = lat; r->lat_max_t = t; }
	}
	if (r->trace && t >= r->next_trace)
	{
		fprintf(r->trace, "%.3f %.1f %u %d %.5f %.5f\n", t, lat * 1000.0 / FEED_RATE, g_feed.count,
		        device_queued(), g_feed.ratio, g_feed.rate);
		r->next_trace = t + 0.25;
	}
}

/** @brief Print the run's engagement, latency, splices and pitch, then the verdict.
 * @param r The run.
 * @return 0 PASS, 2 FAIL, 3 refusal. */
int report(const Run* r)
{
	const double ms = 1000.0 / FEED_RATE;
	const int bound = g_feed.target + g_feed.deadband + SIM_BURST;
	const int natural = (int)ceil(SIM_AMP * 2.0 * M_PI * 660.0 / FEED_RATE);
	printf("span speed=%.3f start=%.1f dur=%.1f drift=%+.4f seed=%u\n", r->span.speed, r->span.start, r->span.dur,
	       r->drift, r->seed);
	printf("engagement span_vbls=%llu samples=%llu periods=%llu\n", (unsigned long long)r->span_vbls,
	       (unsigned long long)r->samples, (unsigned long long)g_feed.pulls);
	printf("latency bound=%d frames (%.1f ms) max_after_settle=%.0f frames (%.1f ms) at t=%.2f s; "
	       "peak_any=%.1f ms at t=%.2f s\n",
	       bound, bound * ms, r->lat_max, r->lat_max * ms, r->lat_max_t, r->peak_any * ms, r->peak_any_t);
	printf("feed underruns=%llu overflow=%llu discarded=%llu splices=%llu splice_step_max=%d natural_step=%d\n",
	       (unsigned long long)g_feed.underruns, (unsigned long long)g_feed.overflow,
	       (unsigned long long)g_feed.discarded, (unsigned long long)r->splices, r->splice_worst, natural);
	PsyX_SPUFeed_Stats st;
	PsyX_SPUFeed_GetStats(&st);
	printf("pitch span_extreme=%+.1fc after_settle_extreme=%+.1fc run_median=%+.2fc run_p99=%.2fc run_max=%+.2fc\n",
	       r->cents_span, r->cents_after, st.pitch_med_c, st.pitch_p99_c, st.pitch_max_c);
	if (!r->span_vbls || !r->samples)
	{
		printf("verdict=REFUSED (zero engagement: span_vbls=%llu samples=%llu)\n",
		       (unsigned long long)r->span_vbls, (unsigned long long)r->samples);
		return 3;
	}
	const int pass = r->lat_max <= bound;
	printf("verdict=%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 2;
}

} // namespace

/** @brief Run the synthetic producer and device through the configured span and report the post-span latency, the splices and the verdict.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 PASS, 2 FAIL, 3 refusal, 64 usage. */
int main(int argc, char** argv)
{
	static Run r;
	if (!parse_run(&r, argc, argv)) return 64;
	r.trace = r.trace_path ? fopen(r.trace_path, "w") : nullptr;
	if (r.trace) fputs("t_s latency_ms fifo dev ratio rate\n", r.trace);
	PsyX_SPUFeed_Simulate(r.drift, r.seed, r.stall);
	PsyX_SPUFeed_Prepare();
	long long grid = 1000000000LL, last = grid;   /* clock, ns */
	PsyX_SPUFeed_SimRunTo(grid);
	PsyX_SPUFeed_SetHold(0);
	double grid_frac = 0.0;
	for (uint64_t vbl = 0;; vbl++)
	{
		const double t = vbl / SIM_VBL_HZ;   /* producer time at nominal speed, s */
		if (t >= r.seconds) break;
		const int in_span = t >= r.span.start && t < r.span.start + r.span.dur;
		const long long at = grid > last ? grid : last;
		const long long push = at + llround(PsyX_SPUFeed_SimProducerDelay(at) * 1e9);
		run_device(&r, t, push);
		push_vbl(&r, in_span);
		sample(&r, t, in_span);
		last = push;
		grid_frac += 1e9 / SIM_VBL_HZ / (in_span ? r.span.speed : 1.0);
		const long long step = (long long)grid_frac;
		grid += step;
		grid_frac -= (double)step;
	}
	if (r.trace) fclose(r.trace);
	return report(&r);
}
