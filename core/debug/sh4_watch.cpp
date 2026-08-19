/*
	SH-4 watches and traps. See sh4_watch.h for what each one is and when to reach for it.

	Everything here is off unless its environment variable is set, and every armed path is
	guarded by a single already-loaded bool so the cost when idle is one predictable branch.
*/
#include "sh4_watch.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sh4watch
{

// Instructions executed since boot. Every record is stamped with it, because wall-clock time
// is meaningless inside an emulator running at a variable speed, while this is a monotonic
// measure of how far the guest has actually got.
static u64 instrCount;

static u32 currentPc()
{
	// The pc has already advanced past the instruction being executed by the time a hook
	// runs, so step back one instruction word to name the instruction responsible.
	return p_sh4rcb->cntx.pc - 2;
}

// ---- execution trap: SH4TRAP=0xaddr[,0xaddr] ------------------------------------------
static bool trapTried;
static u32 trapAddr[8];
static int trapCount;
static u64 trapHits, trapMax = 64;
static FILE *trapFile;

static void trapInit()
{
	trapTried = true;
	const char *e = getenv("SH4TRAP");
	if (e == nullptr || *e == '\0')
		return;
	for (const char *p = e; *p != '\0' && trapCount < 8; )
	{
		trapAddr[trapCount++] = (u32)strtoul(p, nullptr, 0);
		const char *c = strchr(p, ',');
		if (c == nullptr)
			break;
		p = c + 1;
	}
	const char *f = getenv("SH4TRAP_FILE");
	trapFile = (f != nullptr && *f != '\0') ? fopen(f, "w") : stderr;
	const char *m = getenv("SH4TRAP_MAX");
	if (m != nullptr && *m != '\0')
		trapMax = strtoull(m, nullptr, 10);
}

// ---- polled value watch: SH4TRACE_WRWATCH=0xaddr ---------------------------------------
// Polls ONE 32-bit word once per instruction and reports whoever changed it. Cannot miss a
// writer whatever path the write took, which is exactly why it exists alongside the range
// watch below. `pr` is reported too: for a value set inside a small leaf function, the return
// address is what identifies the caller that actually decided it.
static bool valTried;
static u32 valAddr, valPrev;
static u64 valHits, valMax = 32, valAfter;
static FILE *valFile;

static void valInit()
{
	valTried = true;
	const char *a = getenv("SH4TRACE_WRWATCH");
	if (a == nullptr || *a == '\0')
		return;
	valAddr = (u32)strtoul(a, nullptr, 0);
	const char *m = getenv("SH4TRACE_WRWATCH_MAX");
	if (m != nullptr && *m != '\0')
		valMax = strtoull(m, nullptr, 10);
	const char *f = getenv("SH4TRACE_WRWATCH_AFTER");
	if (f != nullptr && *f != '\0')
		valAfter = strtoull(f, nullptr, 10);
	valPrev = ReadMem32(valAddr);
	fprintf(stderr, "[wrw] armed on %08x, initial value %08x\n", valAddr, valPrev);
	fflush(stderr);
	valFile = stderr;
}

void perInstruction()
{
	instrCount++;

	if (!trapTried)
		trapInit();
	if (trapCount != 0 && trapHits < trapMax)
	{
		const u32 pc = currentPc();
		for (int i = 0; i < trapCount; i++)
			if (pc == trapAddr[i])
			{
				const Sh4Context &c = p_sh4rcb->cntx;
				fprintf(trapFile, "[trap] pc=%08x instr=%llu pr=%08x r4=%08x r5=%08x r6=%08x\n",
						pc, (unsigned long long)instrCount, c.pr, c.r[4], c.r[5], c.r[6]);
				fflush(trapFile);
				trapHits++;
				break;
			}
	}

	if (!valTried)
		valInit();
	if (valAddr != 0 && valHits < valMax && instrCount >= valAfter)
	{
		const u32 now = ReadMem32(valAddr);
		if (now != valPrev)
		{
			const Sh4Context &c = p_sh4rcb->cntx;
			fprintf(valFile, "[wrw] %08x  %08x -> %08x  by pc=%08x  instr=%llu  pr=%08x\n",
					valAddr, valPrev, now, currentPc(), (unsigned long long)instrCount, c.pr);
			fflush(valFile);
			valPrev = now;
			valHits++;
		}
	}
}

// ---- range watches: SH4TRACE_RDWATCH_LO/_HI and SH4TRACE_WRWATCH_LO/_HI -----------------
//
// A per-pc summary matters more than the raw stream. The stream says a range was touched;
// the summary says by whom, how often, and between which instructions - which is the
// difference between "something reads this" and "this reader stopped after boot".
struct PcRow { u32 pc; u64 first, last, n; };

struct Range
{
	bool tried = false;
	u32 lo = 0, hi = 0;
	u32 skipLo = 0, skipHi = 0;   // a pc range kept OUT of the raw log (see below)
	u64 kept = 0, max = 400;
	FILE *out = nullptr;
	char path[512] = { 0 };
	PcRow pcs[256];
	int npc = 0;
	u64 nextSidecar = 0;

	void arm(const char *loEnv, const char *hiEnv, const char *outEnv,
			 const char *skipLoEnv, const char *skipHiEnv, const char *maxEnv)
	{
		tried = true;
		const char *l = getenv(loEnv), *h = getenv(hiEnv);
		if (l == nullptr || *l == '\0' || h == nullptr || *h == '\0')
			return;
		lo = (u32)strtoul(l, nullptr, 0);
		hi = (u32)strtoul(h, nullptr, 0);
		const char *o = getenv(outEnv);
		if (o != nullptr && *o != '\0') {
			out = fopen(o, "w");
			snprintf(path, sizeof(path), "%s", o);
		}
		else
			out = stderr;
		// WITHOUT A SKIP RANGE THE LOG IS ALL memcpy. A game's own copy loops touch input
		// buffers every frame, and the record budget is gone before the interesting reader
		// ever runs. The skipped pc range is still COUNTED in the summary, just not printed.
		const char *sl = getenv(skipLoEnv), *sh = getenv(skipHiEnv);
		if (sl != nullptr && *sl != '\0' && sh != nullptr && *sh != '\0') {
			skipLo = (u32)strtoul(sl, nullptr, 0);
			skipHi = (u32)strtoul(sh, nullptr, 0);
		}
		const char *m = getenv(maxEnv);
		if (m != nullptr && *m != '\0')
			max = strtoull(m, nullptr, 10);
	}

	void note(u32 pc)
	{
		for (int i = 0; i < npc; i++)
			if (pcs[i].pc == pc) { pcs[i].last = instrCount; pcs[i].n++; return; }
		if (npc < 256) {
			pcs[npc].pc = pc; pcs[npc].first = instrCount;
			pcs[npc].last = instrCount; pcs[npc].n = 1; npc++;
		}
	}

	// Rewrite the summary to a sidecar periodically. Stopping an emulator means killing it,
	// and a summary that exists only at clean exit is a summary that is normally lost - a
	// whole instrumented session was thrown away learning that. Opened ONCE and rewritten in
	// place: re-creating the file on every dump makes Windows Defender inspect it each time,
	// which stalled two emulator instances badly enough to freeze the guest.
	void sidecar()
	{
		if (path[0] == '\0')
			return;
		static FILE *f;
		if (f == nullptr) {
			char p[560];
			snprintf(p, sizeof(p), "%s.sum", path);
			f = fopen(p, "w");
			if (f == nullptr) { path[0] = '\0'; return; }
		}
		fseek(f, 0, SEEK_SET);
		fprintf(f, "# per-pc summary at instr=%llu\n", (unsigned long long)instrCount);
		for (int i = 0; i < npc; i++)
			fprintf(f, "PC %08x n=%llu first=%llu last=%llu\n", pcs[i].pc,
					(unsigned long long)pcs[i].n, (unsigned long long)pcs[i].first,
					(unsigned long long)pcs[i].last);
		fflush(f);
	}

	void dump(const char *what)
	{
		if (out == nullptr)
			return;
		fprintf(out, "\n# %s per-pc summary: pc, count, first instruction, last instruction\n",
				what);
		for (int i = 0; i < npc; i++)
			fprintf(out, "PC %08x n=%llu first=%llu last=%llu\n", pcs[i].pc,
					(unsigned long long)pcs[i].n, (unsigned long long)pcs[i].first,
					(unsigned long long)pcs[i].last);
		fflush(out);
	}
};

static Range rd, wr;

void onRead(u32 addr)
{
	if (!rd.tried)
		rd.arm("SH4TRACE_RDWATCH_LO", "SH4TRACE_RDWATCH_HI", "SH4TRACE_RDWATCH_OUT",
			   "SH4TRACE_RDWATCH_SKIPLO", "SH4TRACE_RDWATCH_SKIPHI", "SH4TRACE_RDWATCH_MAX");
	if (rd.out == nullptr)
		return;
	const u32 p = addr & 0x1FFFFFFFu;
	if (p < rd.lo || p >= rd.hi)
		return;
	if (instrCount >= rd.nextSidecar) {
		rd.nextSidecar = instrCount + 100000000ull;
		rd.sidecar();
	}
	const u32 pc = currentPc();
	if (rd.skipHi != rd.skipLo && pc - rd.skipLo < rd.skipHi - rd.skipLo) {
		rd.note(pc);
		return;
	}
	if (rd.kept < rd.max) {
		// pr and r1/r2 make a copy self-identifying: pr is the caller that invoked the copy,
		// and r1/r2 are a typical memcpy's destination and source cursors. Without them a
		// watch on a copied-from range names only the copy routine, which tells you nothing.
		const Sh4Context &c = p_sh4rcb->cntx;
		fprintf(rd.out, "R %08x pc=%08x pr=%08x r1=%08x r2=%08x instr=%llu\n",
				p, pc, c.pr, c.r[1], c.r[2], (unsigned long long)instrCount);
		fflush(rd.out);
		rd.kept++;
	}
	rd.note(pc);
}

void onWrite(u32 addr)
{
	if (!wr.tried)
		wr.arm("SH4TRACE_WRWATCH_LO", "SH4TRACE_WRWATCH_HI", "SH4TRACE_WRWATCH_OUT",
			   "SH4TRACE_WRWATCH_SKIPLO", "SH4TRACE_WRWATCH_SKIPHI", "SH4TRACE_WRWATCH_MAX");
	if (wr.out == nullptr)
		return;
	const u32 p = addr & 0x1FFFFFFFu;
	if (p < wr.lo || p >= wr.hi)
		return;
	const u32 pc = currentPc();
	if (wr.kept < wr.max) {
		fprintf(wr.out, "W %08x pc=%08x instr=%llu\n", p, pc,
				(unsigned long long)instrCount);
		fflush(wr.out);
		wr.kept++;
	}
	wr.note(pc);
}

void dumpAll()
{
	rd.dump("read");
	wr.dump("write");
	rd.sidecar();
}

}
