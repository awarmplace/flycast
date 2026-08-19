/*
	SH-4 watches and traps: the instruments that answer "who touched this?"

	This is a deliberately small, self-contained module. The research fork it descends from
	carries the same ideas inside a 135 KB file entangled with a native-code dispatcher and a
	dozen census counters; none of that is wanted here, and rez keeps its own fork for that
	work (see BRANCHES.md). What survives is only what has actually answered questions:

	  SH4TRAP            an execution trap - report when the pc reaches an address
	  SH4TRACE_WRWATCH   a POLLED value watch - report who CHANGED one 32-bit word
	  SH4TRACE_RDWATCH_* a range READ watch  - every read inside a range, with the reading pc
	  SH4TRACE_WRWATCH_* a range WRITE watch - every write inside a range, with the writing pc

	All of it is inert unless its environment variable is set, and all of it only works on the
	interpreter (Dynarec.Enabled = no), because that is where the hooks live.

	WHY BOTH KINDS OF WRITE WATCH, since picking the wrong one has cost this project real time:
	SH4TRACE_WRWATCH (singular) POLLS one address once per instruction and reports whoever
	changed its value. It cannot miss a writer, whatever path the write took - DMA, store
	queue, anything. SH4TRACE_WRWATCH_LO/_HI hooks the interpreter's store path instead, so it
	sees the pc directly but is blind to writes that do not go through it. Use the singular one
	for "who set this word", the range one for "what happens in this region".
*/
#pragma once
#include "types.h"

namespace sh4watch
{

// Called once per instruction from the interpreter. Drives the trap and the polled watch,
// and maintains the instruction counter the watches timestamp with.
void perInstruction();

// Called from the interpreter's memory macros. Cheap no-ops unless a range is armed.
void onRead(u32 addr);
void onWrite(u32 addr);

// Write the per-pc summaries. Called at shutdown; the read watch also writes a sidecar
// periodically while running, because a hard kill is the normal way to stop an emulator and
// a summary that only exists at exit is a summary that is usually lost.
void dumpAll();

}
