/* tests/unit/spual_interp_test.cpp - the SPU mixer's 4-point gaussian
 * interpolation of stock voices, sample-override voices across a loop seam,
 * and host clips, each against an independent evaluation of the psx-spx
 * formula over the sequence the voice plays.
 *
 * PsyX_SPUAL.cpp is compiled into this TU so its static interpolation and
 * host-clip mixing run directly. Its other PsyCross dependencies (log,
 * output feed, XA) are stubbed; the OpenAL device calls link but never run. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"     // warning-ratchet: PsyCross source compiled in unchanged; its stubs leave parameters unused
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"  // warning-ratchet: PsyCross source compiled in unchanged; ADPCM decode history GCC cannot prove set
#include "../../third_party/PsyCross/src/audio/PsyX_SPUAL.cpp"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <vector>

/** @brief PsyCross stub. */
void PsyX_Log(const char* fmt, ...) { (void)fmt; }
/** @brief PsyCross stub. */
void PsyX_Log_Info(const char* fmt, ...) { (void)fmt; }
/** @brief PsyCross stub. */
void PsyX_Log_Error(const char* fmt, ...) { (void)fmt; }
/** @brief PsyCross stub. */
int PsyX_SPUFeed_Open(void) { return 0; }
/** @brief PsyCross stub. */
void PsyX_SPUFeed_Close(void) {}
/** @brief PsyCross stub. */
void PsyX_SPUFeed_Push(const short* frames, int n) { (void)frames; (void)n; }
/** @brief PsyCross stub. */
void PsyX_SPUFeed_Pump(void) {}
/** @brief PsyCross stub. */
void PsyX_XA_Init(void) {}

/** @brief psx-spx 4-point gaussian evaluated term by term from kGauss.
 * @param i Interpolation index, 0..FFh.
 * @param x Four consecutive samples, oldest first.
 * @return The interpolated sample. */
static int32_t ref_gauss(int i, const int32_t x[4])
{
	return (((int32_t)kGauss[0x0FF - i] * x[0]) >> 15) + (((int32_t)kGauss[0x1FF - i] * x[1]) >> 15) +
	       (((int32_t)kGauss[0x100 + i] * x[2]) >> 15) + (((int32_t)kGauss[0x000 + i] * x[3]) >> 15);
}

/** @brief Deterministic test PCM: a full-scale pseudo-random sequence.
 * @param n Sample count.
 * @param seed Generator seed.
 * @return The samples. */
static std::vector<short> make_pcm(int n, uint32_t seed)
{
	std::vector<short> v((size_t)n);
	for (int k = 0; k < n; k++)
	{
		seed = seed * 1664525u + 1013904223u;
		v[(size_t)k] = (short)(seed >> 16);
	}
	return v;
}

/** @brief Stock voice: SPUAL_Voice_Gauss equals the formula at every counter of a block.
 * @return Failure count. */
static int test_stock(void)
{
	SPUALVoice v;
	memset(&v, 0, sizeof v);
	const std::vector<short> pcm = make_pcm(31, 7u);
	for (int k = 0; k < 31; k++) v.decoded[k] = pcm[(size_t)k];
	int fail = 0;
	for (uint32_t c = 0; c < (28u << 12); c += 0x37)
	{
		v.counter = c;
		const int k = (int)(c >> 12) + 3;
		const int32_t x[4] = { v.decoded[k - 3], v.decoded[k - 2], v.decoded[k - 1], v.decoded[k] };
		if (SPUAL_Voice_Gauss(&v) != ref_gauss((c >> 4) & 0xFF, x)) fail++;
	}
	if (fail) fprintf(stderr, "FAIL stock: %d counters differ\n", fail);
	return fail;
}

enum { kCount = 100, kLs = 40, kLl = 50 };   /**< Loop case: PCM length, window start, window length. */

/** @brief Played-sequence sample `j` of the loop case: the PCM up to the window's end, then the window repeated; 0 before the start.
 * @param pcm The PCM.
 * @param j Sample index.
 * @return The sample. */
static int32_t loop_seq(const std::vector<short>& pcm, int64_t j)
{
	if (j < 0) return 0;
	return pcm[(size_t)(j < kLs + kLl ? j : kLs + (j - kLs) % kLl)];
}

/** @brief Whether the four taps at played-sequence index `u` straddle a window end.
 * @param u The `older` tap's index.
 * @return 1 for a seam output. */
static int loop_is_seam(int64_t u)
{
	const int64_t rel = u - 1 - kLs;
	return u + 2 >= kLs + kLl && rel >= 0 && (rel % kLl) >= kLl - 3;
}

/** @brief One rate of the loop case: steps a looping override voice through four window lengths, comparing every output with the formula.
 * @param pcm The PCM.
 * @param rate The PCM's rate.
 * @return Failure count. */
static int loop_case(std::vector<short>& pcm, unsigned rate)
{
	SPUALOverridePCM p = { 1u, pcm.data(), kCount, rate };
	SPUALVoice v;
	memset(&v, 0, sizeof v);
	v.ov_loops = 1;
	v.ov_flat = 1;
	v.ov_loop_start = kLs;
	v.ov_loop_len = kLl;
	const uint32_t step = SPUAL_Override_Step(&v, &p);
	int ls, ll;
	SPUAL_Override_LoopWindow(&v, &p, &ls, &ll);
	if (ls != kLs || ll != kLl) { fprintf(stderr, "FAIL loop window %d+%d\n", ls, ll); return 1; }
	const int64_t bound = (int64_t)(kLs + 2 * kLl) << 16;
	int seams = 0, bad = 0, unbounded = 0;
	for (int64_t upos = 0; (upos >> 16) < kLs + 4 * kLl; upos += step)
	{
		const int64_t u = upos >> 16;
		const int32_t x[4] = { loop_seq(pcm, u - 1), loop_seq(pcm, u), loop_seq(pcm, u + 1), loop_seq(pcm, u + 2) };
		seams += loop_is_seam(u);
		bad += SPUAL_Override_At(&p, v.ov_pos, ls, ll) != ref_gauss((int)((upos >> 8) & 0xFF), x);
		v.ov_pos += step;
		SPUAL_Override_Wrap(&v, &p);
		unbounded += v.ov_pos >= bound;
	}
	printf("loop seam %u Hz: %d seam outputs checked, %d differ, %d positions past the bound\n", rate, seams, bad,
	       unbounded);
	return bad + unbounded + (seams == 0);
}

/** @brief Looping override: every output across three wraps equals the formula over the played sequence (the PCM to the window's end, then the window repeated).
 *
 * The window [40, 90) ends before the PCM's 100 samples, and the rates
 * 11025 and 8000 Hz give exact and inexact steps. A seam output is one whose
 * taps straddle a window end; a rate fails when none was checked.
 * @return Failure count. */
static int test_loop_seam(void)
{
	std::vector<short> pcm = make_pcm(kCount, 11u);
	return loop_case(pcm, 11025u) + loop_case(pcm, 8000u);
}

/** @brief Clip sample `j` of channel `sc` from an interleaved PCM of `ch` channels; 0 outside the clip.
 * @param pcm The PCM.
 * @param frames Frame count.
 * @param ch Channel count.
 * @param sc Channel.
 * @param j Frame index.
 * @return The sample. */
static int32_t clip_at(const std::vector<short>& pcm, int frames, int ch, int sc, int64_t j)
{
	return (j < 0 || j >= frames) ? 0 : pcm[(size_t)(j * ch + sc)];
}

/** @brief One host clip of `ch` channels at 22050 Hz: SPUAL_Host_Sample adds the formula over each channel until the clip ends, then 0.
 * @param ch Channel count, 1 or 2.
 * @return Failure count. */
static int clip_case(int ch)
{
	const int frames = 64;
	const unsigned rate = 22050u;
	std::vector<short> pcm = make_pcm(frames * ch, 23u + (uint32_t)ch);
	const int id = PsyX_SPUAL_HostBufferCreate(pcm.data(), frames, ch, rate);
	if (id <= 0 || PsyX_SPUAL_HostPlay(id, 1.0f, 0) != 0) { fprintf(stderr, "FAIL host play\n"); return 1; }
	const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100u);
	int checked = 0, bad = 0;
	for (int64_t pos = 0; (pos >> 16) < frames; pos += step, checked++)
	{
		int32_t out[2] = { 0, 0 };
		SPUAL_Host_Sample(&out[0], &out[1], 0);
		const int64_t u = pos >> 16;
		for (int c = 0; c < 2; c++)
		{
			const int sc = ch == 2 ? c : 0;
			const int32_t x[4] = { clip_at(pcm, frames, ch, sc, u - 1), clip_at(pcm, frames, ch, sc, u),
			                       clip_at(pcm, frames, ch, sc, u + 1), clip_at(pcm, frames, ch, sc, u + 2) };
			bad += out[c] != ref_gauss((int)((pos >> 8) & 0xFF), x);
		}
	}
	int32_t l = 0, r = 0;
	SPUAL_Host_Sample(&l, &r, 0);
	const int after = (l || r) ? 1 : 0;
	printf("host clip %d ch: %d outputs checked, %d samples differ, output after the end %d\n", ch, checked, bad,
	       after);
	PsyX_SPUAL_HostStopAll();
	return bad + after;
}

/** @brief Host clips, mono and stereo, through SPUAL_Host_Sample.
 * @return Failure count. */
static int test_host_clip(void)
{
	g_SpuMutex = SDL_CreateMutex();
	return clip_case(1) + clip_case(2);
}

/** @brief Runs the stock, loop-seam and host-clip cases.
 * @return 0 when all pass. */
int main(void)
{
	const int fail = test_stock() + test_loop_seam() + test_host_clip();
	printf(fail ? "spual_interp_test: FAIL\n" : "spual_interp_test: PASS\n");
	return fail ? 1 : 0;
}
