#include "sh4watch.h"

#include "hw/sh4/sh4_if.h"   // Sh4cntx

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sh4watch
{

// Initialised before main, so the hot path in mmu_WriteMem is a single test of
// an already-resident bool when no watch is configured.
bool enabled = getenv("SH4WATCH") != nullptr;

namespace
{

struct Range
{
	u32 lo;
	u32 hi;      // exclusive
};

std::vector<Range> ranges;
FILE *fp = nullptr;
long hits = 0;
long limit = 2000;
bool ready = false;
bool watchAll = false;
u64 matchValue = 0;
bool haveValue = false;
u32 matchPc = 0;
bool havePc = false;

void parse()
{
	ready = true;

	const char *spec = getenv("SH4WATCH");
	if (spec == nullptr)
	{
		enabled = false;
		return;
	}
	// SH4WATCH=all logs every write until the limit. Use it to find out which
	// address space the writes are actually in before setting a real watch. A
	// watch on the wrong space logs nothing and looks exactly like "no code
	// writes this", which is the most misleading possible result.
	if (strcmp(spec, "all") == 0)
		watchAll = true;
	const char *maxs = getenv("SH4WATCH_MAX");
	if (maxs != nullptr)
		limit = strtol(maxs, nullptr, 0);

	// SH4WATCH_VALUE logs only writes that store this exact value.
	//
	// Without it a wide sweep is useless. Watching all 33 KB of DWLOBBY's data
	// through one login produced 6000 lines, of which 5392 were a clear loop
	// storing zero to 26 addresses, and the budget was gone before the menu was
	// even opened. Hunting for "which address receives the value 7" is the
	// normal shape of these questions, so filter at the source.
	const char *vals = getenv("SH4WATCH_VALUE");
	if (vals != nullptr)
	{
		matchValue = strtoull(vals, nullptr, 0);
		haveValue = true;
	}

	// SH4WATCH_PC logs writes made by ONE instruction, whatever address they
	// touch. This answers "who called this function", which a write watch
	// cannot otherwise reach in a module whose calls are PC-relative branches
	// that a linear disassembly cannot follow.
	//
	// Point it at a function's `sts.l pr,@-r15` prologue instruction. That
	// instruction stores the CALLER'S return address, so the logged value is
	// the caller. No halting, and it works while the game is being played.
	const char *pcs = getenv("SH4WATCH_PC");
	if (pcs != nullptr)
	{
		matchPc = (u32)strtoul(pcs, nullptr, 0);
		havePc = true;
	}

	// "addr" watches one byte. "addr:len" watches a span, which is how you
	// watch a whole structure without knowing which field moves first.
	const char *p = spec;
	while (*p != '\0')
	{
		char *end = nullptr;
		u32 lo = (u32)strtoul(p, &end, 0);
		if (end == p)
			break;
		u32 len = 1;
		if (*end == ':')
		{
			p = end + 1;
			len = (u32)strtoul(p, &end, 0);
			if (len == 0)
				len = 1;
		}
		ranges.push_back({ lo, lo + len });
		if (*end == ',')
			end++;
		p = end;
	}
	if (ranges.empty() && !watchAll && !havePc)
	{
		enabled = false;
		return;
	}
	// A PC filter on its own means "every write this instruction makes".
	if (ranges.empty() && havePc)
		watchAll = true;

	const char *path = getenv("SH4WATCH_FILE");
	if (path == nullptr)
		path = "sh4watch.log";
	fp = fopen(path, "w");
	if (fp == nullptr)
	{
		enabled = false;
		return;
	}
	fprintf(fp, "# sh4 write watch\n");
	for (const Range &r : ranges)
		fprintf(fp, "# watching 0x%08x .. 0x%08x\n", r.lo, r.hi);
	fprintf(fp, "# columns: hit, pc, address, size, value\n");
	fflush(fp);
}

}	// namespace

void write(u32 vaddr, u64 data, int size)
{
	if (!ready)
		parse();
	if (!enabled || fp == nullptr)
		return;
	if (hits >= limit)
		return;
	if (haveValue && data != matchValue)
		return;
	if (havePc && (Sh4cntx.pc - 2) != matchPc)
		return;

	// A write overlaps a watched byte when the written span covers it. A
	// 32-bit store two bytes below a watched byte still changes it, and that
	// is exactly the case a naive equality test misses.
	if (watchAll)
	{
		const u32 pc = Sh4cntx.pc - 2;
		hits++;
		fprintf(fp, "%ld pc=0x%08x addr=0x%08x size=%d value=0x%llx\n",
				hits, pc, vaddr, size, (unsigned long long)data);
		if (hits >= limit)
			fprintf(fp, "# limit of %ld reached\n", limit);
		fflush(fp);
		return;
	}

	const u32 lo = vaddr;
	const u32 hi = vaddr + (u32)size;
	for (const Range &r : ranges)
	{
		if (lo < r.hi && r.lo < hi)
		{
			// Sh4cntx.pc has already advanced past the instruction being
			// executed, the same convention sh4_trace.cpp documents, so the
			// storing instruction is at pc - 2.
			const u32 pc = Sh4cntx.pc - 2;
			hits++;
			fprintf(fp, "%ld pc=0x%08x addr=0x%08x size=%d value=0x%llx\n",
					hits, pc, vaddr, size, (unsigned long long)data);
			if (hits >= limit)
				fprintf(fp, "# limit of %ld reached, no more lines\n", limit);
			fflush(fp);
			return;
		}
	}
}

}	// namespace sh4watch

namespace sh4watch
{

bool trapEnabled = getenv("SH4TRAP") != nullptr;

namespace
{
std::vector<u32> traps;
FILE *tfp = nullptr;
long thits = 0;
long tlimit = 200;
bool tready = false;
u32 skipR0 = 0;
bool haveSkipR0 = false;

void tparse()
{
	tready = true;
	const char *spec = getenv("SH4TRAP");
	if (spec == nullptr) { trapEnabled = false; return; }
	const char *p = spec;
	while (*p != '\0')
	{
		char *end = nullptr;
		u32 a = (u32)strtoul(p, &end, 0);
		if (end == p) break;
		traps.push_back(a);
		if (*end == ',') end++;
		p = end;
	}
	if (traps.empty()) { trapEnabled = false; return; }
	const char *m = getenv("SH4TRAP_MAX");
	if (m != nullptr) tlimit = strtol(m, nullptr, 0);
	// SH4TRAP_SKIPR0 suppresses hits whose r0 equals this value. A function
	// called every frame returns the same thing thousands of times while
	// nothing is happening, and the interesting hit is the one that differs.
	// Without this the budget is spent long before the moment of interest.
	const char *sk = getenv("SH4TRAP_SKIPR0");
	if (sk != nullptr) { skipR0 = (u32)strtoul(sk, nullptr, 0); haveSkipR0 = true; }
	const char *path = getenv("SH4TRAP_FILE");
	if (path == nullptr) path = "sh4trap.log";
	tfp = fopen(path, "w");
	if (tfp == nullptr) { trapEnabled = false; return; }
	fprintf(tfp, "# sh4 execution trap\n");
	for (u32 a : traps) fprintf(tfp, "# trapping 0x%08x\n", a);
	fflush(tfp);
}
}

void trap(u32 pc)
{
	if (!tready) tparse();
	if (!trapEnabled || tfp == nullptr || thits >= tlimit) return;
	for (u32 a : traps)
	{
		if (a != pc) continue;
		if (haveSkipR0 && Sh4cntx.r[0] == skipR0) return;
		thits++;
		fprintf(tfp, "%ld pc=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x "
				"r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x pr=0x%08x\n",
				thits, pc,
				Sh4cntx.r[0], Sh4cntx.r[1], Sh4cntx.r[2], Sh4cntx.r[3],
				Sh4cntx.r[4], Sh4cntx.r[5], Sh4cntx.r[6], Sh4cntx.r[7],
				Sh4cntx.pr);
		if (thits >= tlimit) fprintf(tfp, "# limit reached\n");
		fflush(tfp);
		return;
	}
}

}
