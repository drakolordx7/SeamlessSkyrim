#include "Skyrim/Bridge/Commands.h"

#include "Skyrim/Bridge/CommandSupport.h"
#include "Skyrim/Bridge/AgentMode.h"
#include "Skyrim/Bridge/FrameHooks.h"

using namespace Bridge::Support;

namespace
{
	json Ping(const json&)
	{
		return {
			{ "plugin", SKSE::GetPluginName() },
			{ "version", SKSE::GetPluginVersion().string() },
			{ "runtime", REL::Module::get().version().string() },
		};
	}

	json State(const json&)
	{
		auto* ui = RE::UI::GetSingleton();
		json  state = {
            { "mainMenu", ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME) },
            { "loading", ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) },
            { "console", ui && ui->IsMenuOpen(RE::Console::MENU_NAME) },
            { "paused", ui && ui->GameIsPaused() },
            { "walk", FrameHooks::WalkStatus() },
		};
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		state["inGame"] = cell && !state["mainMenu"].get<bool>() && !state["loading"].get<bool>();
		if (cell) {
			auto* world = player->GetWorldspace();
			state["player"] = {
				{ "pos", Vec(player->GetPosition()) },
				// Degrees, Skyrim convention: 0 = north (+Y), clockwise.
				{ "headingDeg", player->GetAngleZ() * 180.0f / std::numbers::pi_v<float> },
				{ "cell", CellJson(cell) },
				{ "worldspace", world ? Hex(world->GetFormID()) : "" },
			};
		}
		return state;
	}

	json Console(const json& a_args)
	{
		const auto command = a_args.at("command").get<std::string>();
		auto*      factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
		auto*      script = factory ? factory->Create() : nullptr;
		if (!script) {
			throw std::runtime_error("cannot create a console script");
		}
		script->SetCommand(command);
		// Commands that need a reference either name it ("player." / "<refid>.") or pass "ref".
		script->CompileAndRun(a_args.contains("ref") ? RequireRef(a_args) : nullptr);
		delete script;
		logger::info("bridge: console '{}'", command);
		return { { "executed", command } };
	}

	json LoadGame(const json& a_args)
	{
		const auto name = a_args.at("name").get<std::string>();
		RE::BGSSaveLoadManager::GetSingleton()->Load(name.c_str());
		return { { "loading", name } };
	}

	json SaveGame(const json& a_args)
	{
		RequirePlayer();
		const auto name = a_args.at("name").get<std::string>();
		RE::BGSSaveLoadManager::GetSingleton()->Save(name.c_str());
		return { { "saved", name } };
	}

	json Activate(const json& a_args)
	{
		auto* player = RequirePlayer();
		auto* ref = RequireRef(a_args);
		const bool activated = ref->ActivateRef(player, 0, nullptr, 1, false);
		return { { "activated", activated }, { "ref", Hex(ref->GetFormID()) } };
	}

	json MoveTo(const json& a_args)
	{
		auto* player = RequirePlayer();
		if (a_args.contains("ref")) {
			player->MoveTo(RequireRef(a_args));
		} else {
			const auto& p = a_args.at("pos");
			player->SetPosition(RE::NiPoint3{ p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() }, true);
		}
		return { { "pos", Vec(player->GetPosition()) } };
	}

	json WalkTo(const json& a_args)
	{
		RequirePlayer();
		FrameHooks::WalkRequest request;
		if (a_args.contains("ref")) {
			request.target = RequireRef(a_args)->GetHandle();
		} else {
			const auto& p = a_args.at("pos");
			// Steering is 2D, so z is optional.
			request.point = { p.at(0).get<float>(), p.at(1).get<float>(), p.size() > 2 ? p.at(2).get<float>() : 0.0f };
		}
		request.stopDistance = a_args.value("stopDistance", 64.0f);
		request.run = a_args.value("run", false);
		request.timeoutSeconds = a_args.value("timeoutSeconds", 30.0f);
		FrameHooks::StartWalk(request);
		return FrameHooks::WalkStatus();
	}

	json WalkStop(const json&)
	{
		FrameHooks::StopWalk("stopped by request");
		return FrameHooks::WalkStatus();
	}

	// A RenderDoc capture of the next frame, through the API of the renderdoc.dll that Community
	// Shaders loads when its "Enable RenderDoc Capture" is on: its own trigger is a hotkey, which a
	// game running in the background never gets. Returns the captures RenderDoc has written so far.
	json RenderDocCapture(const json&)
	{
		auto* module = GetModuleHandleW(L"renderdoc.dll");
		using GetApi_t = int (*)(int, void**);
		auto* getApi = module ? reinterpret_cast<GetApi_t>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
		void** api = nullptr;
		constexpr int kVersion_1_1_2 = 10102;
		if (!getApi || !getApi(kVersion_1_1_2, reinterpret_cast<void**>(&api)) || !api) {
			throw std::runtime_error("RenderDoc isn't loaded (Community Shaders: Enable RenderDoc Capture, then restart the game)");
		}
		// RENDERDOC_API_1_1_2, in renderdoc_app.h's order: GetNumCaptures 13, GetCapture 14, TriggerCapture 15.
		using GetNumCaptures_t = std::uint32_t (*)();
		using GetCapture_t = std::uint32_t (*)(std::uint32_t, char*, std::uint32_t*, std::uint64_t*);
		using TriggerCapture_t = void (*)();
		reinterpret_cast<TriggerCapture_t>(api[15])();
		json captures = json::array();
		const auto count = reinterpret_cast<GetNumCaptures_t>(api[13])();
		for (std::uint32_t i = 0; i < count; ++i) {
			char          path[1024]{};
			std::uint32_t length = sizeof(path);
			if (reinterpret_cast<GetCapture_t>(api[14])(i, path, &length, nullptr)) {
				captures.push_back(path);
			}
		}
		return { { "triggered", true }, { "captures", captures } };
	}

	json Screenshot(const json& a_args)
	{
		const auto path = a_args.at("path").get<std::string>();
		return FrameHooks::CaptureScreenshot(std::filesystem::path(path).wstring(), 5s);
	}
}

namespace Bridge
{
	void AddGameCommands(CommandTable& a_commands)
	{
		a_commands.insert({
			{ "ping", { Ping, false } },
			{ "state", { State } },
			{ "console", { Console } },
			{ "load_game", { LoadGame } },
			{ "save_game", { SaveGame } },
			{ "activate", { Activate } },
			{ "move_to", { MoveTo } },
			{ "walk_to", { WalkTo } },
			{ "walk_stop", { WalkStop } },
			// Off the main thread: XAudio2 takes volume changes from any thread, so this also works
			// while the game is busy loading.
			{ "audio", { [](const json& a) { AgentMode::SetMuted(a.at("muted").get<bool>()); return json{ { "muted", AgentMode::Muted() } }; }, false } },
			// Diagnostics: third-person camera zoom and where the camera ends up relative to the player.
			{ "camera", { [](const json& a) {
				 auto* camera = RE::PlayerCamera::GetSingleton();
				 auto* third = camera ? skyrim_cast<RE::ThirdPersonState*>(camera->currentState.get()) : nullptr;
				 if (third && a.contains("zoom")) {
					 third->targetZoomOffset = a.at("zoom").get<float>();
				 }
				 auto*      player = RE::PlayerCharacter::GetSingleton();
				 auto*      world = RE::Main::WorldRootCamera();
				 const auto eye = world ? world->world.translate : RE::NiPoint3{};
				 const auto pos = player->GetPosition();
				 return json{
					 { "state", camera && camera->currentState ? static_cast<int>(camera->currentState->id) : -1 },
					 { "targetZoom", third ? third->targetZoomOffset : 0.0f },
					 { "currentZoom", third ? third->currentZoomOffset : 0.0f },
					 { "posOffsetActual", third ? json{ third->posOffsetActual.x, third->posOffsetActual.y, third->posOffsetActual.z } : json() },
					 { "camera", { eye.x, eye.y, eye.z } },
					 { "horizontalDistance", std::hypot(eye.x - pos.x, eye.y - pos.y) },
					 { "heightAbovePlayer", eye.z - pos.z },
				 };
			 } } },
			// Tests: the camera at the player's head or behind them, without a key press (which would
			// need the game window in front).
			{ "camera_view", { [](const json& a) {
				 auto* camera = RE::PlayerCamera::GetSingleton();
				 if (a.value("firstPerson", true)) {
					 camera->ForceFirstPerson();
				 } else {
					 camera->ForceThirdPerson();
				 }
				 return json{ { "firstPerson", camera->IsInFirstPerson() } };
			 } } },
			// Tests: the activate key, without the game window in front (PlayerCharacter::ActivatePickRef,
			// on whatever the crosshair is on).
			{ "use_pick", { [](const json&) {
				 auto* player = RequirePlayer();
				 auto* pick = RE::CrosshairPickData::GetSingleton();
				 const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
				 static REL::Relocation<void (*)(RE::PlayerCharacter*)> activatePickRef{ REL::RelocationID(39471, 40548) };
				 activatePickRef(player);
				 return json{ { "target", target ? std::format("{:08X}", target->GetFormID()) : "" } };
			 } } },
			// A key press handed to the game's input directly ({"code": DirectInput scan code,
			// "repeat"}): no window focus needed, nothing typed into other windows.
			{ "key", { [](const json& a) {
				 AgentMode::QueueKey(a.at("code").get<std::uint32_t>(), a.value("repeat", 1));
				 return json{ { "queued", a.at("code").get<std::uint32_t>() } };
			 }, false } },
			// Waits for the next Present; running it on the main thread would deadlock the frame.
			{ "screenshot", { Screenshot, false } },
			{ "renderdoc_capture", { RenderDocCapture } },
			// The next `frames` presented frames, every one, as JPEGs in `directory` (which must
			// exist), written once the last is taken.
			{ "capture_frames", { [](const json& a) {
				 // Each frame is held as a copy of the back buffer until all are taken: 33 MB at 4K.
				 const int frames = std::clamp(a.value("frames", 16), 1, 60);
				 if (!FrameHooks::CaptureFrames(std::filesystem::path(a.at("directory").get<std::string>()).wstring(), frames)) {
					 throw std::runtime_error("screenshot hook not installed");
				 }
				 return json{ { "frames", frames } };
			 } } },
			// The first frames of every crossing and merge from now on, each in a folder of its own
			// under `directory`; without one, or with an empty one, off.
			{ "capture_events", { [](const json& a) {
				 const auto directory = a.value("directory", std::string());
				 FrameHooks::SetEventCapture(std::filesystem::path(directory).wstring());
				 return json{ { "directory", directory } };
			 } } },
			// Diagnostics: put any loaded reference somewhere (also in a cell the player isn't in).
			{ "ref_set_position", { [](const json& a) {
				 auto*       ref = RequireRef(a);
				 const auto& p = a.at("pos");
				 ref->SetPosition(RE::NiPoint3{ p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() });
				 return json{ { "pos", Vec(ref->GetPosition()) } };
			 } } },
		});
	}
}
