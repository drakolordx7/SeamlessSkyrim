#include "Skyrim/Plugin/Install.h"

#include "Skyrim/Audio/DoorwayAudio.h"
#include "Skyrim/Audio/DoorwayFilter.h"
#include "Skyrim/Bridge/AgentMode.h"
#include "Skyrim/Bridge/Bridge.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Engine/Paths.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Plugin/Frame.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/EngineHooks.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/StreetLOD.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Render/Enb.h"
#include "Skyrim/Render/Frame.h"
#include "Skyrim/Render/Native/Native.h"
#include "Skyrim/Spaces/RemoteCell.h"
#include "Skyrim/Spaces/Tuning.h"
#include "Skyrim/UI/Menu.h"

namespace Plugin
{
	namespace
	{
		bool hooksInstalled = false;

		// These patch engine code in the draw, cull, physics and travel paths. With portals off they
		// did nothing yet were all there, one more thing to clash with other mods' hooks (as with
		// Precision's in the physics step).
		void InstallHooks()
		{
			// Precision patches the engine's world step loop (ID 77851 + 0x407) with a pre-step hook
			// that takes its own locks for whichever world is stepped; stepping a second world in the
			// step job deadlocked the game in that hook (main thread waiting for the job, ID 35409).
			// Without stepping, NPCs behind the door don't move physically until the player is there.
			// See Settings kStepRemoteHavok for the modes; the default is 2 with Precision loaded
			// (see RemoteCell::StepAtEndOfFrame), else 1.
			const bool stepRemote = Settings::Int(Settings::Id::kStepRemoteHavok) != 0;
			// Before anything asks whether a room can be merged (MergedView::Mergeable).
			Render::Enb::Detect();
			Render::Native::Install();
			Spaces::RemoteCell::InstallStepHook(stepRemote);
			Spaces::RemoteCell::InstallGridHook();
			Render::Frame::Install();
			Render::Frame::Prewarm();
			Pictures::Prewarm();
			Portal::Travel::Install();
			Portal::CameraLimit::Install();
			Portal::StreetLOD::Install();
			Portal::MergedCull::Install();
			Portal::Doors::InstallPromptHook();
			Portal::EngineHooks::Install();
			// The low-pass the live room's sound is heard through; only worth hooking the sound engine for with the sound on.
			if (Audio::Doorway::Enabled()) {
				Audio::Filter::Install();
			}
		}
	}

	void Install()
	{
		Settings::Load();
		Portal::Manager::Install();
		// The doors of the loaded game and the scheduler that decides what is held for them, whether or
		// not portals are on: the manager's releases (a save, a load) reset it.
		Graph::Install();
		Portal::Schedule::Install();
		Audio::Doorway::Install();
		Pictures::SetPictureHeight(Settings::Int(Settings::Id::kDoorwayPictureHeight));
		Spaces::LoadTuning();
		hooksInstalled = Portal::Manager::Enabled() || Diag::AgentMode();
		if (hooksInstalled) {
			InstallHooks();
			logger::info("portal: manager installed, {} ({})", Portal::Manager::Enabled() ? "enabled" : "disabled", Paths::PluginDirectory().string());
			Portal::NpcTravel::Install();
		} else {
			logger::info("portal: off in {}; no hooks installed (switching on needs a restart)", Paths::PluginDirectory().string());
		}
		UI::Menu::Register();
		Frame::Install();
		if (AgentMode::Enabled()) {
			Bridge::ForceAlwaysActive();
			AgentMode::InstallInputGuards();
			AgentMode::InstallAutoLoad();
		}
	}

	bool HooksInstalled()
	{
		return hooksInstalled;
	}
}
