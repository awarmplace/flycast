// Guest memory write watch.
//
// Why this exists: the hardest recurring question in the Sega Rally 2 network
// work is "which code writes this byte, and when". It came up five times in one
// day: what writes state+4, what sets the launch gate at +0xe8, what makes the
// client believe it is in a team, what sets bit 4 of the lobby status word, and
// what clears the list mode flags.
//
// Both existing methods failed. Hand disassembly loses alignment, because these
// Windows CE modules interleave code with literal pools and jump tables, so a
// linear sweep reads data as instructions and misses the branch that matters.
// The GDB stub answers the question but halting the guest frequently wedges it,
// so every read costs the whole run and cannot be done while a person is
// driving the menus.
//
// A write watch has neither problem. It never halts, it reports the exact
// writing instruction, and it works while the game is being played.
//
// The hook sits in mmu_WriteMem, which is the one place that still has the
// VIRTUAL address. Sega Rally 2 runs with the full MMU on, and the addresses we
// care about are Windows CE virtual addresses, so a hook placed after
// translation would report physical addresses and be useless for this.
//
// Control:
//   SH4WATCH=0x019d2784,0x019d2908:4   comma separated, each addr or addr:len
//   SH4WATCH_MAX=2000                  stop logging after this many hits
//   SH4WATCH_FILE=sh4watch.log         output file
//
// The line limit is not decoration. An unbounded log in this tree reached 2.9 GB
// in one session.
#pragma once
#include "types.h"

namespace sh4watch
{
// Set once, before main, from the environment. The hot path tests only this.
extern bool enabled;

// Out of line. Never called unless a watch is configured.
void write(u32 vaddr, u64 data, int size);

inline void onWrite(u32 vaddr, u64 data, int size)
{
	if (enabled)
		write(vaddr, data, size);
}

// Execution trap. SH4TRAP=addr[,addr...] logs every general register when one
// of those addresses executes.
//
// Why this is needed on top of the write watch: a return value that is only
// TESTED is never stored, so a write watch cannot see it. The HRESULT from
// SetConnectionSettings is read in r0 by `tst r0,r0` and then discarded, and
// which error it is decides what to fix. The GDB stub can read it, but halting
// wedges the guest, which cost three runs in one session.
//
// This never halts. The cost when no trap is set is one test of a bool.
extern bool trapEnabled;
void trap(u32 pc);

inline void onExec(u32 pc)
{
	if (trapEnabled)
		trap(pc);
}
}
