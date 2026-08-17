/*
	Flycast Driver - a TCP control surface for automating Flycast.

	This file is part of the flycast-driver overlay. It is licensed under the
	GPL v2 or later, matching Flycast itself.
*/
#pragma once

namespace driver
{

#ifdef USE_DRIVER

// Start the control server. Reads the listen port from the FLYCAST_DRIVER_PORT
// environment variable (default 6510). A port of 0 disables the server.
void init();
void term();

// Called on the emulation thread once per vblank. Drives frame counting and
// any pending frame/predicate waits.
void onVBlank();

// Called on the emulation thread immediately after the maple bus has sampled
// the input globals. Input holds are timed in maple polls rather than vblanks,
// because a poll is the only point at which the guest actually observes a
// button. Deliberately does no job execution: running emulator commands from
// inside a maple DMA transfer would be re-entrant.
void onMaplePoll();

#else

inline static void init() {}
inline static void term() {}
inline static void onVBlank() {}
inline static void onMaplePoll() {}

#endif

}
