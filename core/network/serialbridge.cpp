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
#include "serialbridge.h"
#include "cfg/option.h"
#include <cstdlib>
#include <cstring>

namespace net::modbba
{

std::string SerialBridgeService::endpoint()
{
	// The setting comes first, so a user can configure this in the GUI or in
	// emu.cfg under [network] ModemBridge. The environment variable stays as
	// an override, which is what the first proof of concept used and what
	// scripts that launch several instances still rely on.
	const char *e = getenv("MODEMBRIDGE");
	if (e != nullptr && *e != '\0')
		return std::string(e);
	return config::ModemBridge.get();
}

bool SerialBridgeService::start()
{
	const std::string ep = endpoint();
	if (ep.empty())
		return false;

	std::string host = ep;
	std::string port = "7654";
	const size_t colon = ep.rfind(':');
	if (colon != std::string::npos) {
		host = ep.substr(0, colon);
		port = ep.substr(colon + 1);
	}
	if (host.empty())
		host = "127.0.0.1";

	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *result = nullptr;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr)
	{
		ERROR_LOG(MODEM, "MODEMBRIDGE: cannot resolve %s:%s", host.c_str(), port.c_str());
		return false;
	}

	sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (!VALID(sock))
	{
		freeaddrinfo(result);
		ERROR_LOG(MODEM, "MODEMBRIDGE: cannot create socket");
		return false;
	}
	// Connect while the socket still blocks. The server is a local process that
	// is already listening, so this returns at once. A non-blocking connect
	// here would need a second state machine for no benefit.
	if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) != 0)
	{
		freeaddrinfo(result);
		closesocket(sock);
		sock = INVALID_SOCKET;
		ERROR_LOG(MODEM, "MODEMBRIDGE: cannot connect to %s:%s. Is the server running?",
				host.c_str(), port.c_str());
		return false;
	}
	freeaddrinfo(result);

	set_non_blocking(sock);
	set_tcp_nodelay(sock);
	rxBuffer.clear();
	txBuffer.clear();
	reported = false;

	NOTICE_LOG(MODEM, "MODEMBRIDGE: modem byte stream bridged to %s:%s", host.c_str(), port.c_str());
	return true;
}

void SerialBridgeService::stop()
{
	if (VALID(sock))
	{
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	rxBuffer.clear();
	txBuffer.clear();
}

void SerialBridgeService::poll()
{
	if (!VALID(sock))
		return;

	// Rate limit. modemAvailable() is polled continuously from the emulated
	// modem's timing loop, and an unconditional poll puts two system calls
	// inside that loop. Anything waiting to be sent still goes at once.
	if (txBuffer.empty())
	{
		using namespace std::chrono;
		const auto now = steady_clock::now();
		if (now - lastPoll < milliseconds(1))
			return;
		lastPoll = now;
	}

	// Send first. A byte held in txBuffer is a byte the server is waiting for.
	while (!txBuffer.empty())
	{
		u8 b = txBuffer.front();
		const int rc = ::send(sock, (const char *)&b, 1, 0);
		if (rc == 1) {
			txBuffer.pop_front();
			continue;
		}
		if (rc < 0 && (get_last_error() == L_EWOULDBLOCK || get_last_error() == L_EAGAIN))
			break;			// the socket is full. Try again on the next poll.
		if (!reported) {
			ERROR_LOG(MODEM, "MODEMBRIDGE: send failed, error %d", get_last_error());
			reported = true;
		}
		break;
	}

	// Then receive everything the server has ready.
	u8 buf[512];
	while (true)
	{
		const int rc = ::recv(sock, (char *)buf, sizeof(buf), 0);
		if (rc > 0) {
			for (int i = 0; i < rc; i++)
				rxBuffer.push_back(buf[i]);
			if (rc < (int)sizeof(buf))
				break;
			continue;
		}
		if (rc == 0) {
			if (!reported) {
				NOTICE_LOG(MODEM, "MODEMBRIDGE: the server closed the connection");
				reported = true;
			}
			break;
		}
		if (get_last_error() == L_EWOULDBLOCK || get_last_error() == L_EAGAIN)
			break;			// nothing to read. This is the normal case.
		if (!reported) {
			ERROR_LOG(MODEM, "MODEMBRIDGE: recv failed, error %d", get_last_error());
			reported = true;
		}
		break;
	}
}

void SerialBridgeService::writeModem(u8 b)
{
	txBuffer.push_back(b);
	poll();
}

int SerialBridgeService::readModem()
{
	poll();
	if (rxBuffer.empty())
		return -1;
	const u8 b = rxBuffer.front();
	rxBuffer.pop_front();
	return b;
}

int SerialBridgeService::modemAvailable()
{
	poll();
	return (int)rxBuffer.size();
}

}
