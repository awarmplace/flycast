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
#include "serialbridge.h"
#include "emulator.h"
#include "cfg/option.h"

namespace net::modbba
{

static Service *service;
static bool usingDCNet;
static bool usingBridge;

bool start()
{
	// MODEMBRIDGE overrides the configured backend. Some online titles do not
	// speak PPP, so neither existing backend can serve them; the bridge carries
	// the byte stream to a server outside the emulator.
	const bool wantBridge = !SerialBridgeService::endpoint().empty();
	if (service == nullptr || usingDCNet != config::UseDCNet || usingBridge != wantBridge)
	{
		delete service;
		if (wantBridge)
			service = new SerialBridgeService();
		else if (config::UseDCNet)
			service = new DCNetService();
		else
			service = new PicoTcpService();
		usingDCNet = config::UseDCNet;
		usingBridge = wantBridge;
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
