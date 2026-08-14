/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "netservice.h"
#include "net_platform.h"
#include <deque>
#include <string>
#include <chrono>

namespace net::modbba
{

// A modem backend that carries the guest's byte stream to a plain TCP socket,
// with no protocol of its own and no discovery step.
//
// The existing backends assume PPP: PicoTcp answers PPP locally, and DCNet
// tunnels to a fixed hosted service after a UDP discovery handshake. Some
// online titles use the modem link for a bespoke protocol that is not PPP and
// is not served by dcnet.flyca.st (for example Sega Rally 2's Dwango lobby).
//
// This backend lets that server live OUTSIDE the emulator: every byte the guest
// sends goes to the socket, every byte the socket sends goes to the guest. The
// endpoint is chosen with the MODEMBRIDGE environment variable (host:port).
// Unset, the emulator behaves exactly as before.
class SerialBridgeService : public Service
{
public:
	bool start() override;
	void stop() override;

	void writeModem(u8 b) override;
	int readModem() override;
	int modemAvailable() override;

	// The bridge carries the modem byte stream only. A broadband adapter frame
	// has no meaning here, so it is dropped rather than mishandled.
	void receiveEthFrame(const u8 *frame, u32 size) override {}

	// Reads the MODEMBRIDGE setting. Empty when the backend is not selected.
	static std::string endpoint();

private:
	// Moves bytes in both directions. Called from every entry point, because
	// the guest polls this backend from the emulation thread and there is no
	// other thread to do the work.
	void poll();

	sock_t sock = INVALID_SOCKET;
	std::deque<u8> rxBuffer;	// socket -> guest
	std::deque<u8> txBuffer;	// guest -> socket, holds what send() would not take
	bool reported = false;		// a lost connection is reported once, not per byte
	// When poll() last touched the socket. See poll() for why it is limited.
	std::chrono::steady_clock::time_point lastPoll {};
};

}
