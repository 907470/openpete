/* tests/unit/spu_envelope_test.cpp - the SPU level generators of
 * PsyX_SPUEnvelope.h against the reference's measured levels.
 *
 * Usage: spu_envelope_test <vectors> [--prefix P]. The
 * vectors are tests/unit/data/spu-envelope/reference.env (format in its
 * header; tools/spu_envelope_measure.py regenerates it). Each case runs on
 * one envelope and two sweeps (voice 0 left volume, main left volume),
 * driven as PsyX_SPUEnvelope.h states the SPU drives them: the stores
 * between boundary K and K+1 are delivered in order (ADSR words and volume
 * registers at the store, key-on and key-off latched), then boundary K+1
 * runs the envelope tick (unless the phase is off) and the sweep ticks,
 * then applies a latched key-off, then a latched key-on. The polled level
 * after each boundary is compared with the measured value; the tolerance is
 * zero, since the vectors are exact per boundary.
 *
 * --prefix runs only the cases whose names start with P. Built with
 * SPU_ENV_TEST_WRONG, every attack tick adds one to the level: the negative
 * control that shows the comparison fails on a wrong generator. Exit 0 when
 * every case matches; 1 on any mismatch; 2 on a malformed or empty vector
 * file. */
#include "PsyX_SPUEnvelope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

/** @brief One register store of a case. */
struct Event
{
	long at;
	std::string kind;
	unsigned val;
};

/** @brief One measured case: its settings, its stores and its level at every boundary 0..samples. */
struct Case
{
	std::string name, poll;
	unsigned adsr1 = 0, adsr2 = 0, vol_init = 0;
	long samples = 0;
	std::vector<Event> events;
	std::vector<unsigned> level;   // level[k]: the value after boundary k
	long changes = 0;
};

/** @brief Reader of the vector file, one line at a time. */
class Reader
{
public:
	std::vector<Case> cases;

	/** @brief Take one line.
	 * @param line The line.
	 * @return false on a malformed line. */
	bool line(const char* line)
	{
		if (line[0] == '#' || line[0] == '\n')
			return true;
		if (strncmp(line, "case ", 5) == 0)
			return !in_ && start(line);
		if (!in_)
			return false;
		if (strncmp(line, "event ", 6) == 0)
			return event(line);
		if (strncmp(line, "level ", 6) == 0)
			return level(line);
		if (strncmp(line, "run ", 4) == 0)
			return run(line);
		if (strncmp(line, "end", 3) == 0)
			return end();
		return false;
	}

	/** @brief Whether the file ended outside a case block.
	 * @return true when no block is open. */
	bool closed() const { return !in_; }

private:
	Case c_;
	long k_ = 0;
	unsigned v_ = 0;
	bool in_ = false;
	std::vector<long> changed_;

	/** @brief A case header line opens a block. */
	bool start(const char* line)
	{
		char name[128], poll[16];
		if (sscanf(line, "case %127s poll=%15s adsr1=%x adsr2=%x vol_init=%x samples=%ld", name, poll, &c_.adsr1,
		           &c_.adsr2, &c_.vol_init, &c_.samples) != 6 || c_.samples <= 0)
			return false;
		c_.name = name;
		c_.poll = poll;
		in_ = true;
		return true;
	}

	/** @brief An event line adds a store. */
	bool event(const char* line)
	{
		char kind[16];
		long at;
		unsigned val = 0;
		const int n = sscanf(line, "event %ld %15s %x", &at, kind, &val);
		const bool key = strcmp(kind, "kon") == 0 || strcmp(kind, "koff") == 0;
		if (n < 2 || (!key && n != 3))
			return false;
		c_.events.push_back({ at, kind, key ? 0u : val });
		return true;
	}

	/** @brief The level line sets the value before boundary 1. */
	bool level(const char* line)
	{
		if (sscanf(line, "level 0 %x", &v_) != 1)
			return false;
		k_ = 0;
		c_.level.assign((size_t)c_.samples + 1, v_);
		return true;
	}

	/** @brief A run line adds its changes. */
	bool run(const char* line)
	{
		long cnt, dk, dv;
		if (c_.level.empty() || sscanf(line, "run %ld %ld %ld", &cnt, &dk, &dv) != 3 || dk <= 0 || cnt <= 0)
			return false;
		for (long i = 0; i < cnt; i++)
		{
			k_ += dk;
			v_ = (unsigned)((long)v_ + dv) & 0xFFFFu;
			if (k_ > c_.samples)
				return false;
			c_.level[(size_t)k_] = v_;
			changed_.push_back(k_);
		}
		c_.changes += cnt;
		return true;
	}

	/** @brief The end line fills every boundary from the changes and closes the block. */
	bool end()
	{
		if (c_.level.empty())
			return false;
		// Each value holds from its change to the next.
		size_t ci = 0;
		for (long j = 1; j <= c_.samples; j++)
		{
			if (ci < changed_.size() && changed_[ci] == j)
				ci++;
			else
				c_.level[(size_t)j] = c_.level[(size_t)j - 1];
		}
		cases.push_back(c_);
		c_ = Case();
		changed_.clear();
		in_ = false;
		return true;
	}
};

/** @brief Read the vector file.
 * @param path The file.
 * @param out Receives the cases.
 * @return 0, or -1 on a malformed file. */
int load(const char* path, std::vector<Case>& out)
{
	FILE* f = fopen(path, "r");
	if (!f)
	{
		fprintf(stderr, "spu_envelope_test: cannot open %s\n", path);
		return -1;
	}
	Reader r;
	char line[512];
	bool ok = true;
	while (ok && fgets(line, sizeof line, f))
	{
		ok = r.line(line);
		if (!ok)
			fprintf(stderr, "spu_envelope_test: malformed line in %s: %s", path, line);
	}
	fclose(f);
	if (!ok || !r.closed())
		return -1;
	out.swap(r.cases);
	return 0;
}

/** @brief The generators of one case and the latched key stores. */
struct Voice
{
	PsyX_SPUEnvelope env;
	PsyX_SPUSweep voll, mainl;
	bool kon = false, koff = false;
};

/** @brief Deliver one store between two boundaries.
 * @param v The generators.
 * @param ev The store. */
void deliver(Voice& v, const Event& ev)
{
	if (ev.kind == "kon")
		v.kon = true;
	else if (ev.kind == "koff")
		v.koff = true;
	else if (ev.kind == "adsr1")
		PsyX_SPUEnv_SetADSR(&v.env, (uint16_t)ev.val, v.env.adsr2);
	else if (ev.kind == "adsr2")
		PsyX_SPUEnv_SetADSR(&v.env, v.env.adsr1, (uint16_t)ev.val);
	else if (ev.kind == "voll")
		PsyX_SPUSweep_Write(&v.voll, (uint16_t)ev.val);
	else if (ev.kind == "mainl")
		PsyX_SPUSweep_Write(&v.mainl, (uint16_t)ev.val);
}

/** @brief Run one sample boundary: the ticks, then a latched key-off, then a latched key-on.
 * @param v The generators. */
void boundary(Voice& v)
{
	if (PsyX_SPUEnv_Phase(&v.env) != PSYX_SPU_ENV_OFF)
	{
#ifdef SPU_ENV_TEST_WRONG
		const bool attack = PsyX_SPUEnv_Phase(&v.env) == PSYX_SPU_ENV_ATTACK;
#endif
		PsyX_SPUEnv_Tick(&v.env);
#ifdef SPU_ENV_TEST_WRONG
		if (attack && v.env.level < 0x7FFF)
			v.env.level++;
#endif
	}
	PsyX_SPUSweep_Tick(&v.voll);
	PsyX_SPUSweep_Tick(&v.mainl);
	if (v.koff)
		PsyX_SPUEnv_KeyOff(&v.env);
	if (v.kon)
		PsyX_SPUEnv_KeyOn(&v.env);
	v.kon = v.koff = false;
}

/** @brief The polled register's value as the 16-bit register reads it.
 * @param c The case.
 * @param v The generators.
 * @return The value. */
unsigned polled(const Case& c, const Voice& v)
{
	if (c.poll == "voll")
		return (uint16_t)PsyX_SPUSweep_Level(&v.voll);
	if (c.poll == "mainl")
		return (uint16_t)PsyX_SPUSweep_Level(&v.mainl);
	return (uint16_t)PsyX_SPUEnv_Level(&v.env);
}

/** @brief Run one case and compare every boundary, printing the first mismatch.
 * @param c The case.
 * @return Mismatching boundaries. */
long run_case(const Case& c)
{
	Voice v;
	memset(&v.env, 0, sizeof v.env);
	memset(&v.voll, 0, sizeof v.voll);
	memset(&v.mainl, 0, sizeof v.mainl);
	PsyX_SPUEnv_SetADSR(&v.env, (uint16_t)c.adsr1, (uint16_t)c.adsr2);
	PsyX_SPUSweep_Write(&v.voll, (uint16_t)c.vol_init);
	PsyX_SPUSweep_Write(&v.mainl, 0x3FFF);
	long bad = 0, first = -1;
	unsigned want = 0, got = 0;
	size_t ei = 0;
	for (long k = 0; k <= c.samples; k++)
	{
		if (k > 0)
			boundary(v);
		const unsigned g = polled(c, v);
		if (g != c.level[(size_t)k] && bad++ == 0)
		{
			first = k;
			want = c.level[(size_t)k];
			got = g;
		}
		for (; ei < c.events.size() && c.events[ei].at == k; ei++)
			deliver(v, c.events[ei]);
	}
	if (bad)
		printf("  mismatch %-32s %6ld of %6ld boundaries; first at %ld: want %04X got %04X\n", c.name.c_str(), bad,
		       c.samples + 1, first, want, got);
	return bad;
}

} // namespace

/** @brief Load the vectors, run the selected cases and report every mismatching case.
 * @param argc Argument count.
 * @param argv The vector file, then --prefix P.
 * @return 0 on a pass, 1 on a failure, 2 on a malformed or empty vector file. */
int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: spu_envelope_test <vectors> [--prefix P]\n");
		return 2;
	}
	const char* prefix = "";
	for (int i = 2; i + 1 < argc; i += 2)
		if (strcmp(argv[i], "--prefix") == 0)
			prefix = argv[i + 1];
	std::vector<Case> cases;
	if (load(argv[1], cases) != 0)
		return 2;
	long ran = 0, compared = 0, changes = 0, bad = 0;
	int mismatched = 0;
	for (const Case& c : cases)
	{
		if (c.name.compare(0, strlen(prefix), prefix) != 0)
			continue;
		const long b = run_case(c);
		if (b != 0)
		{
			printf("  %s mismatches\n", c.name.c_str());
			mismatched++;
		}
		bad += b;
		ran++;
		compared += c.samples + 1;
		changes += c.changes;
	}
	if (compared == 0 || changes == 0)
	{
		fprintf(stderr, "spu_envelope_test: no case ran (vacuous)\n");
		return 2;
	}
	printf("spu_envelope_test: %ld cases, %ld boundaries compared (%ld measured level changes); "
	       "%ld boundaries mismatch in %d cases\n",
	       ran, compared, changes, bad, mismatched);
	printf(mismatched ? "spu_envelope_test: FAIL\n" : "spu_envelope_test: PASS\n");
	return mismatched ? 1 : 0;
}
