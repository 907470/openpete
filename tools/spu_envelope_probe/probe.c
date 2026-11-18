/* probe.c - SPU envelope probe, a PS-EXE run by the reference emulator.
 *
 * For each case of cases.h (written by tools/spu_envelope_measure.py) the
 * probe programs voice 0 with the case's ADSR words, then polls one level
 * register (the voice's ENVX, its current left volume, or the current main
 * left volume) in a loop of a few dozen CPU cycles, against root counter 2
 * at the system clock, and logs every change of the level with the counter
 * time of the poll that saw it and the time since the poll before. The
 * case's stores (key on, key off, ADSR and volume register writes) happen
 * at whole SPU samples (768 system clocks) after the case's start, which
 * sits in the middle of a sample period: before each case a calibration
 * key-on locates the sample boundaries, so the counter's slow drift
 * against the SPU's sample clock does not accumulate across cases. Interrupts are off for the whole run, so no
 * handler delays a poll.
 *
 * After the last case the log is handed out 63 words at a time: the probe
 * copies a chunk to WINDOW (word 0 the chunk index) and calls probe_anchor,
 * whose address is the capture's record anchor; the capture's words
 * recorder writes WINDOW at every record. The first chunk (index
 * 0xFFFFFFFF) holds the log length, the overflow flag and the calibrated
 * boundary phase. Chunks past the log repeat zeros until the capture has
 * its record count.
 *
 * Register addresses: psx-spx, "Sound Processing Unit (SPU)" and "Timers". */
#include <stdint.h>

typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

#define R16(a) (*(vu16*)(uintptr_t)(a))
#define R32(a) (*(vu32*)(uintptr_t)(a))

#define VOICE0      0x1F801C00u   /* voice 0: volL volR pitch start adsr1 adsr2 envx repeat */
#define MAIN_VOL_L  0x1F801D80u
#define MAIN_VOL_R  0x1F801D82u
#define REV_VOL_L   0x1F801D84u
#define REV_VOL_R   0x1F801D86u
#define KON_LO      0x1F801D88u
#define KON_HI      0x1F801D8Au
#define KOFF_LO     0x1F801D8Cu
#define KOFF_HI     0x1F801D8Eu
#define PMON_LO     0x1F801D90u
#define NON_LO      0x1F801D94u
#define EON_LO      0x1F801D98u
#define XFER_ADDR   0x1F801DA6u
#define XFER_FIFO   0x1F801DA8u
#define SPUCNT      0x1F801DAAu
#define XFER_CTRL   0x1F801DACu
#define SPUSTAT     0x1F801DAEu
#define CD_VOL_L    0x1F801DB0u
#define CD_VOL_R    0x1F801DB2u
#define EXT_VOL_L   0x1F801DB4u
#define EXT_VOL_R   0x1F801DB6u
#define MAIN_CUR_L  0x1F801DB8u
#define VOICE0_CURL 0x1F801E00u
#define T2_VALUE    0x1F801120u
#define T2_MODE     0x1F801124u

#define SAMPLE      768u          /* system clocks per 44.1 kHz SPU sample */
#define BLOCK_ADDR  0x0200u       /* the looping ADPCM block, in 8-byte units */

typedef struct { uint32_t at; uint16_t what, val; } ProbeEvent;
typedef struct
{
	uint16_t poll;      /* 0 ENVX, 1 voice current volume L, 2 main current volume L */
	uint16_t vol_init;  /* voice 0 left volume register before the case */
	uint16_t adsr1, adsr2;
	uint32_t total;     /* samples polled from the case start */
	uint32_t nev;
	ProbeEvent ev[6];
} ProbeCase;

enum { EV_KON = 1, EV_KOFF, EV_ADSR1, EV_ADSR2, EV_VOL_L, EV_MAIN_L };

#include "cases.h"

#define LOG_BASE ((uint32_t*)0x80040000u)
#define LOG_END  ((uint32_t*)0x801F0000u)
#define WINDOW   ((vu32*)0x8003F000u)

static uint32_t* s_log = LOG_BASE;
static uint32_t s_overflow = 1;   /* set to 0 in main; .data, not .bss */
static uint32_t s_t32 = 1;
static uint32_t s_last16 = 1;
static uint32_t s_phase = 1;

/** @brief Counter time in system clocks, extended past root counter 2's 16 bits; called at least every 65536 clocks. */
static inline uint32_t now(void)
{
	const uint32_t c = R32(T2_VALUE) & 0xFFFFu;
	s_t32 += (c - s_last16) & 0xFFFFu;
	s_last16 = c;
	return s_t32;
}

/** @brief Append a two-word record to the log, or set the overflow flag when it is full. */
static void log2w(uint32_t w0, uint32_t w1)
{
	if (s_log + 2 > LOG_END)
	{
		s_overflow = 1;
		return;
	}
	s_log[0] = w0;
	s_log[1] = w1;
	s_log += 2;
}

/** @brief Wait until the counter reaches `t`. */
static void wait_until(uint32_t t)
{
	while ((int32_t)(now() - t) < 0)
		;
}

/** @brief The first sample-middle time at or after `t`. */
static uint32_t next_mid(uint32_t t)
{
	const uint32_t target = (s_phase + SAMPLE / 2u) % SAMPLE;
	return t + (target + SAMPLE - t % SAMPLE) % SAMPLE;
}

/** @brief Upload one 16-byte ADPCM block, flags loop start, end and repeat, by manual FIFO write at BLOCK_ADDR. */
static void upload_block(void)
{
	R16(SPUCNT) = (uint16_t)(R16(SPUCNT) & ~0x30u);
	while (R16(SPUSTAT) & 0x30u)
		now();
	R16(XFER_CTRL) = 0x0004u;
	R16(XFER_ADDR) = BLOCK_ADDR;
	R16(XFER_FIFO) = 0x0700u;    /* shift 0, filter 0; flags 7 */
	for (int i = 0; i < 7; i++)
		R16(XFER_FIFO) = 0x1111u;
	R16(SPUCNT) = (uint16_t)((R16(SPUCNT) & ~0x30u) | 0x10u);
	while (R16(SPUSTAT) & 0x0400u)
		now();
	R16(SPUCNT) = (uint16_t)(R16(SPUCNT) & ~0x30u);
	while (R16(SPUSTAT) & 0x30u)
		now();
}

/** @brief Key voice 0 off with the fastest release and wait for ENVX 0, then set its registers for a case. */
static void voice_reset(uint16_t vol_init, uint16_t adsr1, uint16_t adsr2)
{
	R16(VOICE0 + 0xA) = 0x0000u;
	R16(KOFF_LO) = 1u;
	const uint32_t t0 = now();
	while (R16(VOICE0 + 0xC) != 0 && now() - t0 < 200000u * SAMPLE)
		;
	wait_until(now() + 8u * SAMPLE);
	R16(MAIN_VOL_L) = 0x3FFFu;
	R16(MAIN_VOL_R) = 0x3FFFu;
	R16(VOICE0 + 0x0) = vol_init;
	R16(VOICE0 + 0x2) = 0x3FFFu;
	R16(VOICE0 + 0x4) = 0x1000u;
	R16(VOICE0 + 0x6) = BLOCK_ADDR;
	R16(VOICE0 + 0xE) = BLOCK_ADDR;
	R16(VOICE0 + 0x8) = adsr1;
	R16(VOICE0 + 0xA) = adsr2;
	wait_until(now() + 8u * SAMPLE);
}

/** @brief Perform one case event store. */
static void do_event(const ProbeEvent* e)
{
	switch (e->what)
	{
		case EV_KON:    R16(KON_LO) = 1u; break;
		case EV_KOFF:   R16(KOFF_LO) = 1u; break;
		case EV_ADSR1:  R16(VOICE0 + 0x8) = e->val; break;
		case EV_ADSR2:  R16(VOICE0 + 0xA) = e->val; break;
		case EV_VOL_L:  R16(VOICE0 + 0x0) = e->val; break;
		case EV_MAIN_L: R16(MAIN_VOL_L) = e->val; break;
		default: break;
	}
}

/** @brief Locate the SPU sample boundaries: key on a fast linear attack and take the counter time, modulo one sample, of its first level change. */
static void calibrate(void)
{
	voice_reset(0x3FFFu, 0x000Fu, 0x1FC0u);
	R16(KON_LO) = 1u;
	uint32_t prev = now();
	for (;;)
	{
		const uint32_t t = now();
		if (R16(VOICE0 + 0xC) != 0)
		{
			s_phase = prev % SAMPLE;
			break;
		}
		prev = t;
	}
}

/** @brief Run one case: log its start, the level before its first store, every level change and every event, then its end with the largest poll gap. */
static void run_case(uint32_t idx, const ProbeCase* c)
{
	static const uint32_t kPoll[3] = { VOICE0 + 0xC, VOICE0_CURL, MAIN_CUR_L };
	vu16* const reg = (vu16*)(uintptr_t)kPoll[c->poll];
	calibrate();
	voice_reset(c->vol_init, c->adsr1, c->adsr2);
	const uint32_t t0 = next_mid(now() + 4u * SAMPLE);
	log2w(0xFC000000u | idx, t0);
	wait_until(t0);
	/* The first poll logs the level before the case's first store. */
	uint32_t last = 0xFFFFFFFFu;
	uint32_t prev = now(), maxgap = 0, ei = 0;
	const uint32_t end = c->total * SAMPLE;
	for (;;)
	{
		const uint32_t t = now();
		const uint32_t v = *reg;
		const uint32_t gap = t - prev;
		if (gap > maxgap)
			maxgap = gap;
		if (v != last)
		{
			log2w(((gap > 0xEFFFu ? 0xEFFFu : gap) << 16) | v, t);
			last = v;
		}
		prev = t;
		while (ei < c->nev && t - t0 >= c->ev[ei].at * SAMPLE)
		{
			const uint32_t te = now();
			do_event(&c->ev[ei]);
			log2w(0xF0000000u | ((uint32_t)c->ev[ei].what << 16) | c->ev[ei].val, te);
			ei++;
		}
		if (t - t0 >= end)
			break;
	}
	log2w(0xFE000000u | (maxgap > 0xFFFFFFu ? 0xFFFFFFu : maxgap), now());
}

/** @brief The capture's record anchor: each call is one record of WINDOW. */
__attribute__((noinline)) void probe_anchor(void)
{
	__asm__ volatile("nop");
}

/** @brief One GTE instruction (NCLIP); the capture refuses a run whose record intervals executed none. */
static inline void gte_op(void)
{
	__asm__ volatile("c2 0x1400006");
}

int main(void)
{
	s_overflow = 0;
	s_t32 = 0;
	R32(T2_MODE) = 0;          /* free run at the system clock */
	s_last16 = R32(T2_VALUE) & 0xFFFFu;
	R16(SPUCNT) = 0xC000u;     /* enable, unmute */
	R16(KOFF_LO) = 0xFFFFu;
	R16(KOFF_HI) = 0x00FFu;
	R16(PMON_LO) = 0; R16(PMON_LO + 2) = 0;
	R16(NON_LO) = 0;  R16(NON_LO + 2) = 0;
	R16(EON_LO) = 0;  R16(EON_LO + 2) = 0;
	R16(REV_VOL_L) = 0; R16(REV_VOL_R) = 0;
	R16(CD_VOL_L) = 0;  R16(CD_VOL_R) = 0;
	R16(EXT_VOL_L) = 0; R16(EXT_VOL_R) = 0;
	upload_block();
	for (uint32_t i = 0; i < N_CASES; i++)
		run_case(i, &k_cases[i]);
	voice_reset(0x3FFFu, 0x000Fu, 0x1FC0u);

	const uint32_t n = (uint32_t)(s_log - LOG_BASE);
	WINDOW[0] = 0xFFFFFFFFu;
	WINDOW[1] = n;
	WINDOW[2] = s_overflow;
	WINDOW[3] = s_phase;
	WINDOW[4] = N_CASES;
	for (int i = 5; i < 64; i++)
		WINDOW[i] = 0;
	gte_op();
	probe_anchor();
	for (uint32_t c = 0;; c++)
	{
		WINDOW[0] = c;
		for (uint32_t i = 0; i < 63; i++)
		{
			const uint32_t k = c * 63u + i;
			WINDOW[1 + i] = k < n ? LOG_BASE[k] : 0u;
		}
		gte_op();
		probe_anchor();
	}
}
