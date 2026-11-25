#include "Bridge/Commands.h"

#include "Bridge/CommandSupport.h"
#include "Bridge/FrameHooks.h"
#include "Doors/DoorUse.h"
#include "Doors/Settings.h"
#include "Pictures/DoorwaySnapshot.h"
#include "Portal/MergedCull.h"
#include "Portal/PortalManager.h"
#include "Portal/Travel.h"
#include "Portal/TravelTrace.h"
#include "Render/Frame.h"
#include "Spaces/Tuning.h"

using namespace Bridge::Support;

namespace Bridge
{
	void AddPortalCommands(CommandTable& a_commands)
	{
		a_commands.insert({
			// Tests: the doors opened by a use and the global setting ({"force": 0 none, 1 all open, 2 all
			// shut}, as the menu sets it, for the session).
			{ "doors", { [](const json& a) {
				 if (a.contains("force")) {
					 Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(std::clamp(a.at("force").get<int>(), 0, 2)));
				 }
				 return Portal::DoorUse::Status();
			 } } },
			// As the menu's button: every door's picture deleted, on disk and in memory, so a door is
			// seen as never seen open again (an always-open door shut until its space is there).
			{ "pictures_delete", { [](const json&) {
				 return json{ { "deleted", Portal::Snapshot::DeleteAll() } };
			 } } },
			{ "merge_test", { [](const json&) { return Portal::Manager::MergeTest(); } } },
			{ "portal_merge", { [](const json& a) {
				 if (a.contains("enabled")) {
					 Portal::Manager::SetMergeEnabled(a.at("enabled").get<bool>());
				 }
				 if (a.contains("quietLeftCell")) {
					 Spaces::tuning.quietSecondsLeftCell = a.at("quietLeftCell").get<float>();
				 }
				 if (a.contains("handOver")) {
					 Spaces::tuning.handOverStreet = a.at("handOver").get<bool>();
				 }
				 if (a.contains("stepExterior")) {
					 Spaces::tuning.stepExterior = a.at("stepExterior").get<bool>();
				 }
				 if (a.contains("streetEye")) {
					 Portal::Manager::SetStreetEye(a.at("streetEye").get<bool>());
				 }
				 if (a.contains("ownLighting")) {
					 Portal::Manager::SetMergedOwnLighting(a.at("ownLighting").get<bool>());
				 }
				 return json{ { "enabled", Portal::Manager::MergeEnabled() }, { "adaptation", Portal::Manager::DescribeAdaptation() } };
			 } } },
			// Tests (run_suite's checks): see Portal::Manager::SceneCheck.
			{ "scene_check", { [](const json& a) { return Portal::Manager::SceneCheck(a.value("radius", 3000.0f)); } } },
			{ "merged_shapes", { [](const json& a) {
				 const auto& p = a.at("near");
				 return json{ { "shapes", Portal::Manager::MergedShapesReport({ p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() }, a.value("far", 3000.0f)) } };
			 } } },
			// Tests: the turn and shift of the current door pair (as the in-game menu sets them).
			{ "door_correction", { [](const json& a) {
				 const auto portal = Portal::Manager::CurrentPortal();
				 auto*      source = portal ? RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor) : nullptr;
				 auto*      linked = portal ? RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->linkedDoor) : nullptr;
				 if (!source || !linked) {
					 throw std::runtime_error("no door pair in use");
				 }
				 auto* key = Portal::Doors::CorrectionDoor(source, linked);
				 if (a.contains("turn")) {
					 Portal::Doors::SetCorrection(key->GetFormID(), a.at("turn").get<float>());
				 }
				 if (a.contains("shift")) {
					 const auto& v = a.at("shift");
					 Portal::Doors::SetShift(key->GetFormID(), { v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>() });
				 }
				 const auto shift = Portal::Doors::Shift(key->GetFormID());
				 return json{ { "key", std::format("{:08X}", key->GetFormID()) }, { "turn", Portal::Doors::Correction(key->GetFormID()) },
					 { "shift", { shift.x, shift.y, shift.z } } };
			 } } },
			// Diagnostics / measuring: the merged room's cull (Portal/MergedCull.h), mode 0-3, rooms
			// (through portals) on or off.
			{ "merged_cull", { [](const json& a) {
				 if (a.contains("mode")) {
					 Portal::MergedCull::SetMode(static_cast<Portal::MergedCull::Mode>(std::clamp(a.at("mode").get<int>(), 0, 3)));
				 }
				 if (a.contains("rooms")) {
					 Portal::MergedCull::SetRoomCull(a.at("rooms").get<bool>());
				 }
				 if (a.contains("street")) {
					 Portal::MergedCull::SetStreetMode(static_cast<Portal::MergedCull::Mode>(std::clamp(a.at("street").get<int>(), 0, 2)));
				 }
				 auto result = Portal::MergedCull::Describe();
				 result["frames"] = FrameHooks::FrameStats();
				 return result;
			 } } },
			// Testing without touching the ini: {count} as [Portals] iShownRooms, {pause} as
			// bPauseUnseenRooms.
			{ "shown_rooms", { [](const json& a) {
				 if (a.contains("count")) {
					 Portal::Manager::SetShownRooms(a.at("count").get<int>());
				 }
				 if (a.contains("pause")) {
					 Portal::Manager::SetPauseUnseenRooms(a.at("pause").get<bool>());
				 }
				 auto status = Portal::Manager::Status();
				 return json{ { "shownRooms", status["shownRooms"] }, { "warm", status["warm"] }, { "mergedCull", status["mergedCull"] }, { "frames", FrameHooks::FrameStats() } };
			 } } },
			{ "portal_passes", { [](const json& a) { return Portal::Manager::RemoteRenderPasses(a.value("drop", false)); } } },
			{ "portals_enable", { [](const json& a) {
				 Portal::Manager::SetEnabled(a.value("enabled", true));
				 return Portal::Manager::Status();
			 } } },
			{ "portal_status", { [](const json&) {
				 auto status = Portal::Manager::Status();
				 status["lastFrame"] = ::Render::Frame::LastFrame();
				 status["travel"] = Portal::Travel::Status();
				 return status;
			 } } },
			{ "travel_profile", { [](const json& a) {
				 const auto file = a.at("file").get<std::string>();
				 Portal::TravelTrace::ProfileNext(std::filesystem::path(file));
				 return json{ { "file", file } };
			 } } },
			// Waits for the next rendered frame, like screenshot.
			{ "portal_render_file", { [](const json& a) { return ::Render::Frame::RenderToFile(a.at("path").get<std::string>()); }, false } },
		});
	}
}
