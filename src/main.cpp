#include "Bridge/AgentMode.h"
#include "Bridge/Bridge.h"
#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Plugin/Frame.h"
#include "Portal/Travel.h"
#include "UI/Menu.h"
#include "Probe.h"

// Runs on SE 1.5.97 and on AE from 1.6.629. We read engine structs directly (cells, TES, process
// lists), so declare the post-1.6.629 layout honestly: AE's SKSE then refuses to load us on the
// earlier AE layout, which we never tested, instead of letting us read garbage. SE's SKSE ignores
// this declaration and loads us through SKSEPlugin_Query.
SKSEPluginInfo(
	.Version = REL::Version{ PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH, 0 },
	.Name = PLUGIN_NAME,
	.Author = "bloodline",
	.StructCompatibility = SKSE::StructCompatibility::Dependent,
	.RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility(SKSE::VersionIndependence::AddressLibrary, true))

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			// Event sources, the data handler and the renderer's swap chain all exist by now.
			Portal::Manager::Install();
			if (Portal::Manager::HooksInstalled()) {
				Portal::NpcTravel::Install();
			}
			UI::Menu::Register();
			Plugin::Frame::Install();
			if (AgentMode::Enabled()) {
				Bridge::ForceAlwaysActive();
				AgentMode::InstallInputGuards();
				AgentMode::InstallAutoLoad();
			}
			break;
		// A foreign cell attached under our root must never end up in a save, and a load replaces
		// the world the portal refers to; the manager re-attaches on the next frames anyway.
		case SKSE::MessagingInterface::kSaveGame:
			// The save is written over the next frames; re-attaching right away (seen 0.1 s later)
			// would put the cell back while it's being serialized.
			// Except in a room walked into: let go, it would leave the player standing in the walls
			// of its house, and fall (see Manager::NoteRoomForSave).
			if (!Portal::Manager::NoteRoomForSave()) {
				Portal::Manager::ReleaseAll("saving", 3.0f);
				Portal::Travel::CloseArrivalDoor();
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Portal::Manager::ReleaseAll("loading a save", 3.0f);
			Portal::Manager::ForgetDoors();
			Portal::Travel::ForgetTrip();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Probe::DumpWorldState("game loaded");
			Portal::Manager::EnterRoomAfterLoad();
			break;
		case SKSE::MessagingInterface::kNewGame:
			Probe::DumpWorldState("game loaded");
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	// Every line written through: a game killed on exit (a crash in another mod's shutdown) lost
	// the end of the log, which was where a test's findings were.
	spdlog::default_logger()->flush_on(spdlog::level::trace);
	SKSE::AllocTrampoline(2048);
	if (auto* serialization = SKSE::GetSerializationInterface()) {
		serialization->SetUniqueID('SMLS');
		serialization->SetSaveCallback(Portal::Manager::SaveRoom);
		serialization->SetLoadCallback(Portal::Manager::LoadRoom);
		serialization->SetRevertCallback([](SKSE::SerializationInterface*) { Portal::Manager::ForgetRoom(); });
	}
	logger::info("{} {} loaded", SKSE::GetPluginName(), SKSE::GetPluginVersion().string());

	// Before anything can call the telemetry sink or the frame callbacks.
	Bridge::RegisterTelemetry();
	// Before anything else touches windows: the game window doesn't exist yet at this point.
	AgentMode::InstallFocusGuards();
	AgentMode::InstallMute();
	SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	// Agent sessions only (the MCP server sets SEAMLESS_AGENT): normal play gets no bridge and
	// keeps the game's own alt-tab behaviour.
	if (AgentMode::Enabled()) {
		// Started early so the MCP server can tell "game is starting" from "game not running".
		Bridge::Start();
		// Also forced again at kDataLoaded; this early call covers startup when the agent has
		// already taken focus away from the game.
		Bridge::ForceAlwaysActive();
	}
	logger::info("agent mode: {}", AgentMode::Enabled() ? (AgentMode::Background() ? "on (background)" : "on") : "off");
	return true;
}
