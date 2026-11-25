#include "Portal/TravelTrace.h"

#include "Diag/Probe.h"
#include "Diag/Telemetry.h"
#include "Engine/LoadScreen.h"
#include "Engine/Paths.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Trace
	{
		bool                     running = false;
		Clock::time_point        start;
		Clock::time_point        lastFrame;
		RE::FormID               fromCell = 0;
		RE::FormID               door = 0;
		std::uint32_t            frames = 0;
		double                   worstFrameMs = 0.0;
		double                   arrivedMs = -1.0;
		std::vector<std::string> events;
	};
	Trace trace;
	json  lastResult;

	std::filesystem::path profileFile;
	std::filesystem::path profileAllTo;
	constexpr double      kProfileAllSeconds = 5.0;

	double Ms(Clock::duration a_duration)
	{
		return std::chrono::duration<double, std::milli>(a_duration).count();
	}

	std::string OpenMenus()
	{
		auto*       ui = RE::UI::GetSingleton();
		std::string open;
		for (const std::string_view name : { RE::LoadingMenu::MENU_NAME, RE::FaderMenu::MENU_NAME, std::string_view("LoadWaitSpinner") }) {
			if (ui && ui->IsMenuOpen(name)) {
				open += std::format("{} ", name);
			}
		}
		return open;
	}
}

namespace Portal::TravelTrace
{
	void Install()
	{
		if (!Diag::AgentMode()) {
			return;
		}
		wchar_t folder[MAX_PATH]{};
		const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
		if (::GetPrivateProfileStringW(L"Portals", L"sProfileCrossingsTo", L"", folder, MAX_PATH, ini.c_str()) > 0) {
			profileAllTo = folder;
			std::error_code error;
			std::filesystem::create_directories(profileAllTo, error);
		}
	}

	void Begin(RE::FormID a_door, RE::TESObjectCELL* a_from)
	{
		trace = {};
		trace.running = true;
		trace.start = trace.lastFrame = Clock::now();
		trace.door = a_door;
		Diag::AddCount("crossing", 1.0);
		Diag::CaptureEventFrames(L"crossing", 16);
		if (!profileFile.empty()) {
			Diag::SamplerStart(profileFile, 1.5);
			profileFile.clear();
		} else if (!profileAllTo.empty() && !Diag::SamplerRunning()) {
			static int crossings = 0;
			const auto file = profileAllTo / std::format("crossing_{:02}.txt", ++crossings);
			Diag::SamplerStart(file, kProfileAllSeconds);
			Note(std::format("sampling the main thread into {}", file.string()));
		}
		trace.fromCell = a_from ? a_from->GetFormID() : 0;
		Note(std::format("crossing door {:08X} from {} (menus: {})", trace.door, Probe::DescribeCell(a_from), OpenMenus()));
	}

	void Note(const std::string& a_what)
	{
		const auto at = Ms(Clock::now() - trace.start);
		trace.events.push_back(std::format("{:7.1f} ms: {}", at, a_what));
		logger::info("travel: {:7.1f} ms: {}", at, a_what);
	}

	void Update()
	{
		if (!trace.running) {
			return;
		}
		const auto now = Clock::now();
		const auto frameMs = Ms(now - trace.lastFrame);
		trace.lastFrame = now;
		++trace.frames;
		trace.worstFrameMs = std::max(trace.worstFrameMs, frameMs);
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      cell = player->GetParentCell();
		const auto menus = OpenMenus();
		if (frameMs > 40.0 || Engine::LoadScreenUp() || !menus.empty()) {
			Note(std::format("frame {} took {:.1f} ms, load screen flag {}, menus: {}", trace.frames, frameMs, Engine::LoadScreenUp(), menus));
		}
		if (trace.arrivedMs < 0.0 && cell && cell->GetFormID() != trace.fromCell) {
			trace.arrivedMs = Ms(now - trace.start);
			const auto pos = player->GetPosition();
			Note(std::format("arrived in {} at ({:.0f},{:.0f},{:.0f}) yaw {:.1f}", Probe::DescribeCell(cell), pos.x, pos.y, pos.z, RE::rad_to_deg(player->GetAngleZ())));
		}
		if (Ms(now - trace.start) > 3000.0) {
			trace.running = false;
			lastResult = {
				{ "door", std::format("{:08X}", trace.door) },
				{ "arrivedMs", trace.arrivedMs },
				{ "frames", trace.frames },
				{ "worstFrameMs", trace.worstFrameMs },
				{ "events", trace.events },
			};
			Note(std::format("trace done: arrived after {:.1f} ms, worst frame {:.1f} ms", trace.arrivedMs, trace.worstFrameMs));
		}
	}

	void ProfileNext(const std::filesystem::path& a_file)
	{
		profileFile = a_file;
	}

	bool Running()
	{
		return trace.running;
	}

	const json& Last()
	{
		return lastResult;
	}
}
