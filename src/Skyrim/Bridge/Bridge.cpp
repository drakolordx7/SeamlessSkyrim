#include "Skyrim/Bridge/Bridge.h"

#include "Skyrim/Bridge/AgentMode.h"
#include "Skyrim/Bridge/Commands.h"
#include "Skyrim/Bridge/FrameHooks.h"
#include "Skyrim/Bridge/Perf.h"
#include "Skyrim/Bridge/Sampler.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Render/Present.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <thread>

namespace
{
	// Long enough for heavy commands (cell scans), short enough that a call made during a loading
	// screen, when the main thread doesn't service tasks, fails clearly instead of hanging the agent.
	constexpr auto kMainThreadTimeout = 10s;

	json RunOnMainThread(const Bridge::Command& a_command, const json& a_args)
	{
		// A command given up on is not carried out later: reported as failed, it still ran when the
		// loading screen ended, and a `load_game` or `move_to` from the step before landed in the
		// middle of the next one. Whoever takes it first, the main thread or the timeout, has it.
		enum class Taken { kNo, kByMainThread, kByTimeout };
		struct Call
		{
			std::promise<json> result;
			std::atomic<Taken> taken{ Taken::kNo };
		};
		auto call = std::make_shared<Call>();
		auto future = call->result.get_future();
		SKSE::GetTaskInterface()->AddTask([call, handler = a_command.handler, a_args] {
			if (auto expected = Taken::kNo; !call->taken.compare_exchange_strong(expected, Taken::kByMainThread)) {
				return;
			}
			try {
				call->result.set_value(handler(a_args));
			} catch (...) {
				call->result.set_exception(std::current_exception());
			}
		});
		if (future.wait_for(kMainThreadTimeout) != std::future_status::ready) {
			if (auto expected = Taken::kNo; call->taken.compare_exchange_strong(expected, Taken::kByTimeout)) {
				throw std::runtime_error("timed out waiting for the game's main thread (loading screen?); the command was not carried out");
			}
			// Begun just now: its answer is waited for.
		}
		return future.get();
	}

	std::string Dispatch(const std::string& a_line)
	{
		json reply;
		try {
			const auto request = json::parse(a_line);
			reply["id"] = request.value("id", json());
			const auto name = request.at("cmd").get<std::string>();
			const auto args = request.value("args", json::object());

			const auto& commands = Bridge::GetCommands();
			const auto  it = commands.find(name);
			if (it == commands.end()) {
				throw std::runtime_error(std::format("unknown command '{}'", name));
			}
			reply["result"] = it->second.mainThread ? RunOnMainThread(it->second, args) : it->second.handler(args);
			reply["ok"] = true;
		} catch (const std::exception& e) {
			reply["ok"] = false;
			reply["error"] = e.what();
		}
		return reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
	}

	void ServeClient(SOCKET a_client)
	{
		std::string buffer;
		char        chunk[4096];
		for (;;) {
			const int received = recv(a_client, chunk, sizeof(chunk), 0);
			if (received <= 0) {
				break;
			}
			buffer.append(chunk, static_cast<std::size_t>(received));
			for (auto newline = buffer.find('\n'); newline != std::string::npos; newline = buffer.find('\n')) {
				const auto line = buffer.substr(0, newline);
				buffer.erase(0, newline + 1);
				const auto response = Dispatch(line);
				send(a_client, response.data(), static_cast<int>(response.size()), 0);
			}
		}
		closesocket(a_client);
	}

	void ServerLoop()
	{
		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			logger::error("bridge: WSAStartup failed");
			return;
		}
		// Not inheritable: a crash handler spawned by the game (post_mortem.exe) otherwise inherits
		// the listening socket and keeps the port busy, so the next game start can't bind it.
		const SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
		sockaddr_in  address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(Bridge::kPort);
		// Loopback only: the bridge runs arbitrary console commands, so it must never be reachable
		// from the network.
		inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
		// Keep retrying: the port can still be held briefly by a dying previous instance.
		for (int attempt = 1; bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0; ++attempt) {
			if (attempt == 1 || attempt % 30 == 0) {
				logger::warn("bridge: 127.0.0.1:{} busy (error {}), retrying", Bridge::kPort, WSAGetLastError());
			}
			std::this_thread::sleep_for(2s);
		}
		if (listen(listener, 4) != 0) {
			logger::error("bridge: listen failed (error {})", WSAGetLastError());
			closesocket(listener);
			return;
		}
		logger::info("bridge: listening on 127.0.0.1:{}", Bridge::kPort);

		// Each client on its own thread: the MCP server keeps its connection open for the session, and
		// a test script talking to the bridge at the same time waited for good. Commands still run one
		// at a time, on the main thread.
		for (;;) {
			const SOCKET client = accept(listener, nullptr, nullptr);
			if (client == INVALID_SOCKET) {
				continue;
			}
			logger::info("bridge: client connected");
			std::thread([client] {
				ServeClient(client);
				logger::info("bridge: client disconnected");
			}).detach();
		}
	}
}

namespace Bridge
{
	void ForceAlwaysActive()
	{
		// An agent drives the game while the user works in other windows; without this the game
		// stops its main loop whenever it loses focus and every bridge command times out. Forced
		// here because the setting lives in Skyrim.ini, which Vortex profiles manage and which is
		// easy to put in the wrong file (SkyrimPrefs.ini doesn't work).
		auto* setting = RE::INISettingCollection::GetSingleton()->GetSetting("bAlwaysActive:General");
		if (!setting) {
			logger::warn("bridge: bAlwaysActive:General not found");
			return;
		}
		logger::info("bridge: bAlwaysActive was {}, forcing on", setting->GetBool());
		setting->data.b = true;
	}

	void RegisterTelemetry()
	{
		Diag::agentMode = AgentMode::Enabled();
		Diag::sink = {
			.scopeBegin = Perf::ScopeBegin,
			.scopeEnd = Perf::ScopeEnd,
			.addCount = Perf::AddCount,
			.gpuBegin = Perf::GpuBegin,
			.gpuEnd = Perf::GpuEnd,
			.frameEnd = [] { Perf::FrameEnd(); Sampler::WatchTick(); },
			.samplerStart = Sampler::Start,
			.samplerRunning = Sampler::Running,
			.samplerSetMainThread = Sampler::SetMainThread,
			.captureEventFrames = FrameHooks::CaptureEventFrames,
			.stopWalk = FrameHooks::StopWalk,
			.markArrivalFrame = Render::Present::MarkArrivalFrame,
		};
		FrameHooks::Register();
	}

	void Start()
	{
		std::thread(ServerLoop).detach();
	}
}
