/*
	Copyright 2025 flyinghead

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
#include "netservice.h"
#include "picoppp.h"
#include "dcnet.h"
#include "emulator.h"
#include "cfg/option.h"
#include "rawmodem.h"

namespace net::modbba
{

static Service *service;
static bool usingDCNet;

bool start()
{
	if (service == nullptr || usingDCNet != config::UseDCNet)
	{
		delete service;
		// Games that speak neither PPP nor IP on the match link, and so need the raw byte
		// pipe rather than a TCP/IP stack. Power Stone 2 dials an ISP and speaks PPP for
		// the lobby, then places a SECOND modem call for the fight - and that second call
		// carries a proprietary byte protocol, which is what this backend is for.
		const std::string& gameId = settings.content.gameId;
		if (gameId == "HDR0010"			// Sega Rally 2 (JP)
				|| gameId == "T1218M")	// Power Stone 2 (JP)
			service = new RawModemService();
		else if (config::UseDCNet)
			service = new DCNetService();
		else
			service = new PicoTcpService();
		usingDCNet = config::UseDCNet;
	}
	return service->start();
}

void stop() {
	if (service != nullptr)
		service->stop();
}

void writeModem(u8 b) {
	verify(service != nullptr);
	service->writeModem(b);
}
int readModem() {
	verify(service != nullptr);
	return service->readModem();
}
int modemAvailable() {
	verify(service != nullptr);
	return service->modemAvailable();
}

void receiveEthFrame(const u8 *frame, u32 size) {
	start();
	service->receiveEthFrame(frame, size);
}

}
