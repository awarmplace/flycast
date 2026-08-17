/*
	Flycast Driver - a TCP control surface for automating Flycast.

	Protocol: one request per line, one JSON response per line.
	See docs/protocol.md in the flycast-driver repo.

	This file is part of the flycast-driver overlay. It is licensed under the
	GPL v2 or later, matching Flycast itself.
*/
#include "driver.h"

#ifdef USE_DRIVER

#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "input/gamepad_device.h"
#include "input/gamepad.h"
#include "hw/mem/addrspace.h"
#include "network/net_platform.h"
#include "lua/lua.h"
#include "stdclass.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Implemented in ui/gui.cpp by the driver overlay patch.
bool gui_captureScreenshotToFile(const std::string& path, std::string& error);

namespace driver
{

static constexpr int DEFAULT_PORT = 6510;
// Upper bound on a single command. Long waits are legitimate ("advance 3600"
// is a minute of emulated time), so this is generous; the real protection
// against wedging is the stalled-pump check in dispatch().
static constexpr int MAX_COMMAND_MS = 300000;
// If the emulation thread hasn't pumped for this long, it isn't going to.
static constexpr int STALL_MS = 5000;

// ---------------------------------------------------------------- text utils

static std::string jsonEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s)
	{
		switch (c)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)c < 0x20)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else
				out += c;
			break;
		}
	}
	return out;
}

static std::string okJson(const std::string& rawResult) {
	return "{\"ok\":true,\"result\":" + rawResult + "}";
}
static std::string okString(const std::string& s) {
	return okJson("\"" + jsonEscape(s) + "\"");
}
static std::string errJson(const std::string& msg) {
	return "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}";
}

// Undo the escaping the client applies so a request always fits on one line.
static std::string unescape(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] != '\\' || i + 1 >= s.size()) {
			out += s[i];
			continue;
		}
		switch (s[++i])
		{
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		case '\\': out += '\\'; break;
		default: out += s[i]; break;
		}
	}
	return out;
}

static std::vector<std::string> tokenize(const std::string& s)
{
	std::vector<std::string> out;
	std::istringstream is(s);
	std::string tok;
	while (is >> tok)
		out.push_back(tok);
	return out;
}

// Returns everything after the first `skip` whitespace-separated tokens, raw.
static std::string tailAfter(const std::string& s, int skip)
{
	size_t i = 0;
	for (int n = 0; n < skip; n++)
	{
		while (i < s.size() && isspace((unsigned char)s[i])) i++;
		while (i < s.size() && !isspace((unsigned char)s[i])) i++;
	}
	while (i < s.size() && isspace((unsigned char)s[i])) i++;
	return s.substr(i);
}

// ------------------------------------------------------------- name mappings

struct NamedBit { const char *name; u32 mask; };

static const NamedBit BUTTONS[] = {
	{ "a", DC_BTN_A }, { "b", DC_BTN_B }, { "c", DC_BTN_C }, { "d", DC_BTN_D },
	{ "x", DC_BTN_X }, { "y", DC_BTN_Y }, { "z", DC_BTN_Z },
	{ "start", DC_BTN_START },
	{ "up", DC_DPAD_UP }, { "down", DC_DPAD_DOWN },
	{ "left", DC_DPAD_LEFT }, { "right", DC_DPAD_RIGHT },
	{ "up2", DC_DPAD2_UP }, { "down2", DC_DPAD2_DOWN },
	{ "left2", DC_DPAD2_LEFT }, { "right2", DC_DPAD2_RIGHT },
	{ "reload", DC_BTN_RELOAD }, { "card", DC_BTN_INSERT_CARD },
};

static std::string toLower(std::string s) {
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

// Accepts "a", "a+start", "start|up", or a raw numeric mask ("0x2c" / "44").
static bool parseButtons(const std::string& spec, u32& mask, std::string& error)
{
	mask = 0;
	std::string s = toLower(spec);
	if (s.empty()) {
		error = "empty button spec";
		return false;
	}
	// raw numeric mask
	if (isdigit((unsigned char)s[0]))
	{
		mask = (u32)strtoul(s.c_str(), nullptr, 0);
		return true;
	}
	size_t start = 0;
	while (start <= s.size())
	{
		size_t sep = s.find_first_of("+|,", start);
		std::string name = s.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
		if (!name.empty())
		{
			bool found = false;
			for (const NamedBit& b : BUTTONS)
				if (name == b.name) { mask |= b.mask; found = true; break; }
			if (!found) {
				error = "unknown button '" + name + "'";
				return false;
			}
		}
		if (sep == std::string::npos)
			break;
		start = sep + 1;
	}
	return true;
}

enum class AxisKind { Full, Half };
struct NamedAxis { const char *name; AxisKind kind; int index; };

// index selects which of the arrays below; resolved in setAxis()
static const NamedAxis AXES[] = {
	{ "joyx",  AxisKind::Full, 0 }, { "joyy",  AxisKind::Full, 1 },
	{ "joyrx", AxisKind::Full, 2 }, { "joyry", AxisKind::Full, 3 },
	{ "joy3x", AxisKind::Full, 4 }, { "joy3y", AxisKind::Full, 5 },
	{ "lt",  AxisKind::Half, 0 }, { "rt",  AxisKind::Half, 1 },
	{ "lt2", AxisKind::Half, 2 }, { "rt2", AxisKind::Half, 3 },
};

static bool setAxis(int player, const std::string& name, int value, std::string& error)
{
	std::string n = toLower(name);
	for (const NamedAxis& a : AXES)
	{
		if (n != a.name)
			continue;
		if (a.kind == AxisKind::Full)
		{
			s16 v = (s16)std::max(-32768, std::min(32767, value));
			switch (a.index) {
			case 0: joyx[player] = v; break;
			case 1: joyy[player] = v; break;
			case 2: joyrx[player] = v; break;
			case 3: joyry[player] = v; break;
			case 4: joy3x[player] = v; break;
			case 5: joy3y[player] = v; break;
			}
		}
		else
		{
			u16 v = (u16)std::max(0, std::min(65535, value));
			switch (a.index) {
			case 0: lt[player] = v; break;
			case 1: rt[player] = v; break;
			case 2: lt2[player] = v; break;
			case 3: rt2[player] = v; break;
			}
		}
		return true;
	}
	error = "unknown axis '" + name + "'";
	return false;
}

// ------------------------------------------------------------- driver state

static std::atomic<u64> vblankCount{0};
static std::atomic<u64> maplePollCount{0};
static std::atomic<bool> gameRunning{false};
// steady_clock milliseconds at the last pump(), so a client can tell the
// difference between "still working" and "the emu thread is gone"
static std::atomic<s64> lastPumpMs{0};
static std::mutex gameIdMutex;
static std::string cachedGameId;

// A button hold scheduled to release after it has definitely been sampled.
struct Hold
{
	int player;
	u32 mask;
	u64 releaseAtPoll;
	u64 releaseAtVblankCap;	// safety net if the guest stops polling maple
};
static std::vector<Hold> holds;	// emu thread only

struct Job
{
	std::string request;
	std::promise<std::string> reply;
	bool started = false;
	// pending wait state
	bool waiting = false;
	u64 waitUntilVblank = 0;
	u64 waitUntilPoll = 0;
	u64 deadlineVblank = 0;
	std::string waitExpr;
};

static std::mutex inboxMutex;
static std::deque<std::shared_ptr<Job>> inbox;
static std::vector<std::shared_ptr<Job>> active;	// emu thread only

static std::atomic<bool> serverRunning{false};
static sock_t listenSock = INVALID_SOCKET;
static std::thread acceptThread;

// ------------------------------------------------------------ job execution

static bool checkPlayer(int player, std::string& error)
{
	if (player < 1 || player > 4) {
		error = "player must be 1..4";
		return false;
	}
	return true;
}

static void releaseAllInput(int player)
{
	kcode[player] = ~0u;
	joyx[player] = joyy[player] = 0;
	joyrx[player] = joyry[player] = 0;
	joy3x[player] = joy3y[player] = 0;
	lt[player] = rt[player] = lt2[player] = rt2[player] = 0;
	for (auto it = holds.begin(); it != holds.end(); )
		it = (it->player == player) ? holds.erase(it) : it + 1;
}

#ifdef USE_LUA
static std::string luaResultToJson(const std::string& v)
{
	// numbers and booleans pass through unquoted so the client gets real types
	if (v == "true" || v == "false" || v == "nil")
		return v == "nil" ? "null" : v;
	char *end = nullptr;
	strtod(v.c_str(), &end);
	if (end != nullptr && *end == '\0' && !v.empty())
		return v;
	return "\"" + jsonEscape(v) + "\"";
}
#endif

// Executes a job. Returns a response, or sets job.waiting and returns "".
static std::string executeJob(Job& job)
{
	const std::vector<std::string> tok = tokenize(job.request);
	if (tok.empty())
		return errJson("empty request");
	const std::string cmd = toLower(tok[0]);
	std::string error;

	if (cmd == "press" || cmd == "release" || cmd == "tap")
	{
		if (tok.size() < 3)
			return errJson(cmd + " requires <player> <buttons>");
		int player = atoi(tok[1].c_str());
		if (!checkPlayer(player, error))
			return errJson(error);
		u32 mask = 0;
		if (!parseButtons(tok[2], mask, error))
			return errJson(error);
		player--;

		if (cmd == "release") {
			kcode[player] |= mask;
			return okString("released");
		}
		kcode[player] &= ~mask;	// active low
		if (cmd == "press")
			return okString("pressed");

		// tap: hold for N maple polls, then release. Blocks until released so
		// that when this returns the guest has provably seen the press.
		int polls = tok.size() > 3 ? atoi(tok[3].c_str()) : 4;
		if (polls < 1)
			polls = 1;
		Hold h;
		h.player = player;
		h.mask = mask;
		h.releaseAtPoll = maplePollCount.load() + polls;
		h.releaseAtVblankCap = vblankCount.load() + polls + 120;
		holds.push_back(h);
		job.waiting = true;
		job.waitUntilPoll = h.releaseAtPoll;
		job.waitUntilVblank = 0;
		job.deadlineVblank = h.releaseAtVblankCap + 60;
		return "";
	}
	else if (cmd == "axis")
	{
		if (tok.size() < 4)
			return errJson("axis requires <player> <axis> <value>");
		int player = atoi(tok[1].c_str());
		if (!checkPlayer(player, error))
			return errJson(error);
		if (!setAxis(player - 1, tok[2], atoi(tok[3].c_str()), error))
			return errJson(error);
		return okString("set");
	}
	else if (cmd == "neutral")
	{
		if (tok.size() > 1)
		{
			int player = atoi(tok[1].c_str());
			if (!checkPlayer(player, error))
				return errJson(error);
			releaseAllInput(player - 1);
		}
		else
		{
			for (int p = 0; p < 4; p++)
				releaseAllInput(p);
		}
		return okString("neutral");
	}
	else if (cmd == "advance")
	{
		int frames = tok.size() > 1 ? atoi(tok[1].c_str()) : 1;
		if (frames < 1)
			frames = 1;
		job.waiting = true;
		job.waitUntilVblank = vblankCount.load() + frames;
		job.waitUntilPoll = 0;
		job.deadlineVblank = job.waitUntilVblank + 600;
		return "";
	}
	else if (cmd == "waitfor")
	{
#ifdef USE_LUA
		if (tok.size() < 3)
			return errJson("waitfor requires <timeoutFrames> <luaExpr>");
		int timeout = atoi(tok[1].c_str());
		if (timeout < 1)
			timeout = 600;
		job.waiting = true;
		job.waitExpr = unescape(tailAfter(job.request, 2));
		job.waitUntilVblank = 0;
		job.waitUntilPoll = 0;
		job.deadlineVblank = vblankCount.load() + timeout;
		return "";
#else
		return errJson("waitfor requires a Lua-enabled build");
#endif
	}
	else if (cmd == "eval")
	{
#ifdef USE_LUA
		std::string code = unescape(tailAfter(job.request, 1));
		std::string result;
		if (!lua::evalString(code, result))
			return errJson("lua: " + result);
		return okJson(luaResultToJson(result));
#else
		return errJson("eval requires a Lua-enabled build");
#endif
	}
	else if (cmd == "read")
	{
		if (tok.size() < 3)
			return errJson("read requires <8|16|32> <address>");
		if (!gameRunning.load())
			return errJson("no game running");
		int size = atoi(tok[1].c_str());
		u32 addr = (u32)strtoul(tok[2].c_str(), nullptr, 0);
		u32 v;
		switch (size) {
		case 8:  v = addrspace::readt<u8>(addr); break;
		case 16: v = addrspace::readt<u16>(addr); break;
		case 32: v = addrspace::readt<u32>(addr); break;
		default: return errJson("size must be 8, 16 or 32");
		}
		return okJson(std::to_string(v));
	}
	else if (cmd == "write")
	{
		if (tok.size() < 4)
			return errJson("write requires <8|16|32> <address> <value>");
		if (!gameRunning.load())
			return errJson("no game running");
		int size = atoi(tok[1].c_str());
		u32 addr = (u32)strtoul(tok[2].c_str(), nullptr, 0);
		u32 val = (u32)strtoul(tok[3].c_str(), nullptr, 0);
		switch (size) {
		case 8:  addrspace::writet<u8>(addr, (u8)val); break;
		case 16: addrspace::writet<u16>(addr, (u16)val); break;
		case 32: addrspace::writet<u32>(addr, val); break;
		default: return errJson("size must be 8, 16 or 32");
		}
		return okString("written");
	}
	else if (cmd == "savestate" || cmd == "loadstate")
	{
		int slot = tok.size() > 1 ? atoi(tok[1].c_str()) : config::SavestateSlot;
		if (cmd == "savestate")
			dc_savestate(slot);
		else
			dc_loadstate(slot);
		return okString(cmd + " slot " + std::to_string(slot));
	}
	else if (cmd == "frames")
	{
		return okJson("{\"vblank\":" + std::to_string(vblankCount.load())
				+ ",\"maplePoll\":" + std::to_string(maplePollCount.load()) + "}");
	}
	return errJson("unknown command '" + cmd + "'");
}

// Returns true when a waiting job's condition is now satisfied.
static bool checkWait(Job& job, std::string& response)
{
	const u64 vb = vblankCount.load();
	const u64 poll = maplePollCount.load();

	if (!job.waitExpr.empty())
	{
#ifdef USE_LUA
		std::string result;
		if (!lua::evalString("return (" + job.waitExpr + ")", result)) {
			response = errJson("lua: " + result);
			return true;
		}
		if (result == "true") {
			response = okJson("{\"matched\":true,\"vblank\":" + std::to_string(vb) + "}");
			return true;
		}
#endif
		if (vb >= job.deadlineVblank) {
			response = okJson("{\"matched\":false,\"vblank\":" + std::to_string(vb) + "}");
			return true;
		}
		return false;
	}

	if (job.waitUntilPoll != 0)
	{
		if (poll >= job.waitUntilPoll || vb >= job.deadlineVblank) {
			response = okString("tapped");
			return true;
		}
		return false;
	}

	if (vb >= job.waitUntilVblank) {
		response = okJson("{\"vblank\":" + std::to_string(vb) + "}");
		return true;
	}
	if (vb >= job.deadlineVblank) {
		response = errJson("advance timed out");
		return true;
	}
	return false;
}

// Runs on the emulation thread. Executes new jobs and completes waiting ones.
static void pump()
{
	lastPumpMs.store((s64)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	{
		std::lock_guard<std::mutex> lock(inboxMutex);
		while (!inbox.empty()) {
			active.push_back(inbox.front());
			inbox.pop_front();
		}
	}
	for (auto it = active.begin(); it != active.end(); )
	{
		Job& job = **it;
		std::string response;
		bool done = false;
		try {
			if (!job.started)
			{
				job.started = true;
				response = executeJob(job);
				if (!job.waiting || !response.empty())
					done = true;
				else
					// let a wait that is already satisfied finish in this pump
					done = checkWait(job, response);
			}
			else if (job.waiting) {
				done = checkWait(job, response);
			}
			else {
				done = true;
				response = errJson("internal: job in bad state");
			}
		} catch (const std::exception& e) {
			response = errJson(std::string("exception: ") + e.what());
			done = true;
		} catch (...) {
			response = errJson("unknown exception");
			done = true;
		}
		if (done) {
			try {
				job.reply.set_value(response);
			} catch (const std::future_error&) {
				// client already gave up
			}
			it = active.erase(it);
		}
		else
			++it;
	}
}

static void expireHolds()
{
	const u64 poll = maplePollCount.load();
	const u64 vb = vblankCount.load();
	for (auto it = holds.begin(); it != holds.end(); )
	{
		if (poll >= it->releaseAtPoll || vb >= it->releaseAtVblankCap) {
			kcode[it->player] |= it->mask;
			it = holds.erase(it);
		}
		else
			++it;
	}
}

// --------------------------------------------------------------- networking

// Commands safe to run without the emulation thread, so they still work when
// no game is loaded and nothing is pumping.
static bool handleDirect(const std::string& request, std::string& response)
{
	const std::vector<std::string> tok = tokenize(request);
	if (tok.empty())
		return false;
	const std::string cmd = toLower(tok[0]);

	if (cmd == "ping") {
		response = okString("pong");
		return true;
	}
	if (cmd == "status")
	{
		std::string gameId;
		{
			std::lock_guard<std::mutex> lock(gameIdMutex);
			gameId = cachedGameId;
		}
		response = okJson(std::string("{\"running\":") + (gameRunning.load() ? "true" : "false")
				+ ",\"gameId\":\"" + jsonEscape(gameId) + "\""
				+ ",\"vblank\":" + std::to_string(vblankCount.load())
				+ ",\"maplePoll\":" + std::to_string(maplePollCount.load())
				+ ",\"width\":" + std::to_string(settings.display.width)
				+ ",\"height\":" + std::to_string(settings.display.height)
				+ "}");
		return true;
	}
	if (cmd == "screenshot")
	{
		if (tok.size() < 2) {
			response = errJson("screenshot requires <path>");
			return true;
		}
		std::string path = unescape(tailAfter(request, 1));
		std::string error;
		if (!gui_captureScreenshotToFile(path, error))
			response = errJson(error);
		else
			response = okString(path);
		return true;
	}
	return false;
}

static std::string dispatch(const std::string& request)
{
	std::string response;
	if (handleDirect(request, response))
		return response;

	if (!gameRunning.load())
		return errJson("no game running (command needs the emulation thread)");

	auto job = std::make_shared<Job>();
	job->request = request;
	std::future<std::string> future = job->reply.get_future();
	{
		std::lock_guard<std::mutex> lock(inboxMutex);
		inbox.push_back(job);
	}

	// An abandoned job stays in `active` and is completed later; the shared_ptr
	// keeps it alive, so setting its promise after we return is harmless.
	const auto start = std::chrono::steady_clock::now();
	while (true)
	{
		if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
			return future.get();
		const auto now = std::chrono::steady_clock::now();
		if (!gameRunning.load())
			return errJson("game stopped while the command was in flight");
		const s64 sincePump = (s64)std::chrono::duration_cast<std::chrono::milliseconds>(
				now.time_since_epoch()).count() - lastPumpMs.load();
		if (sincePump > STALL_MS)
			return errJson("emulation thread stalled for " + std::to_string(sincePump)
					+ "ms (paused, or in the menu?)");
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > MAX_COMMAND_MS)
			return errJson("command exceeded " + std::to_string(MAX_COMMAND_MS) + "ms");
	}
}

static void clientThread(sock_t sock)
{
	set_tcp_nodelay(sock);
	std::string buffer;
	char chunk[1024];
	while (serverRunning.load())
	{
		int n = (int)recv(sock, chunk, sizeof(chunk), 0);
		if (n <= 0)
			break;
		buffer.append(chunk, n);
		size_t nl;
		while ((nl = buffer.find('\n')) != std::string::npos)
		{
			std::string line = buffer.substr(0, nl);
			buffer.erase(0, nl + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			std::string response = dispatch(line) + "\n";
			size_t sent = 0;
			while (sent < response.size())
			{
				int w = (int)send(sock, response.data() + sent, (int)(response.size() - sent), 0);
				if (w <= 0)
					break;
				sent += w;
			}
		}
	}
	closesocket(sock);
}

static void acceptLoop()
{
	while (serverRunning.load())
	{
		sockaddr_in addr{};
		socklen_t addrLen = sizeof(addr);
		sock_t client = accept(listenSock, (sockaddr *)&addr, &addrLen);
		if (!VALID(client))
		{
			if (!serverRunning.load())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		std::thread(clientThread, client).detach();
	}
}

// ------------------------------------------------------------------- events

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Start:
	case Event::Resume:
		gameRunning.store(true);
		{
			std::lock_guard<std::mutex> lock(gameIdMutex);
			cachedGameId = settings.content.gameId;
		}
		break;
	case Event::Terminate:
	case Event::Pause:
		// Paused means no vblanks, so no job would ever be pumped. Report it
		// as not-running rather than letting clients block.
		gameRunning.store(false);
		break;
	case Event::VBlank:
		onVBlank();
		break;
	default:
		break;
	}
}

// -------------------------------------------------------------- public API

void init()
{
	int port = DEFAULT_PORT;
	const char *env = getenv("FLYCAST_DRIVER_PORT");
	if (env != nullptr)
		port = atoi(env);
	if (port <= 0) {
		NOTICE_LOG(COMMON, "Driver: disabled (FLYCAST_DRIVER_PORT=%s)", env);
		return;
	}

	listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (!VALID(listenSock)) {
		ERROR_LOG(COMMON, "Driver: cannot create socket");
		return;
	}
	int one = 1;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16)port);
	// loopback only: this is a control channel, never expose it to the network
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::bind(listenSock, (sockaddr *)&addr, sizeof(addr)) != 0
			|| listen(listenSock, 4) != 0)
	{
		ERROR_LOG(COMMON, "Driver: cannot listen on port %d (already in use?)", port);
		closesocket(listenSock);
		listenSock = INVALID_SOCKET;
		return;
	}

	EventManager::listen(Event::Start, emuEventCallback);
	EventManager::listen(Event::Resume, emuEventCallback);
	EventManager::listen(Event::Pause, emuEventCallback);
	EventManager::listen(Event::Terminate, emuEventCallback);
	EventManager::listen(Event::VBlank, emuEventCallback);

	serverRunning.store(true);
	acceptThread = std::thread(acceptLoop);
	NOTICE_LOG(COMMON, "Driver: listening on 127.0.0.1:%d", port);
}

void term()
{
	if (!serverRunning.load())
		return;
	serverRunning.store(false);

	EventManager::unlisten(Event::Start, emuEventCallback);
	EventManager::unlisten(Event::Resume, emuEventCallback);
	EventManager::unlisten(Event::Pause, emuEventCallback);
	EventManager::unlisten(Event::Terminate, emuEventCallback);
	EventManager::unlisten(Event::VBlank, emuEventCallback);

	if (VALID(listenSock)) {
		shutdown(listenSock, SHUT_RD);
		closesocket(listenSock);
		listenSock = INVALID_SOCKET;
	}
	if (acceptThread.joinable())
		acceptThread.join();

	// Fail anything not yet picked up so clients don't wait out the timeout.
	// `active` belongs to the emulation thread and is left alone; jobs there
	// are completed by pump() or time out client-side.
	std::lock_guard<std::mutex> lock(inboxMutex);
	for (auto& job : inbox) {
		try { job->reply.set_value(errJson("emulator shutting down")); }
		catch (const std::future_error&) {}
	}
	inbox.clear();
}

void onVBlank()
{
	vblankCount.fetch_add(1);
	expireHolds();
	pump();
}

void onMaplePoll()
{
	// Only counting and hold expiry here. Jobs are executed from onVBlank, so
	// nothing re-enters the emulator in the middle of a maple DMA transfer.
	// Input set at vblank persists in kcode until released, so it is still
	// guaranteed to be sampled by the polls that follow.
	maplePollCount.fetch_add(1);
	expireHolds();
}

}

#endif	// USE_DRIVER
