#include "Skyrim/Bridge/Commands.h"

#include "Skyrim/Audio/DoorwayAudio.h"
#include "Skyrim/Bridge/CommandSupport.h"
#include "Skyrim/Bridge/FrameHooks.h"
#include "Skyrim/Bridge/PortalDiagnostics.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Portal/Ahead.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Portal/TravelTrace.h"
#include "Skyrim/Render/Frame.h"
#include "Skyrim/Spaces/RemoteCell.h"
#include "Skyrim/Spaces/Tuning.h"

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
				 return json{ { "deleted", Pictures::Cache::DeleteAll() } };
			 } } },
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
				 return json{ { "enabled", Portal::MergedView::MergeEnabled() }, { "adaptation", PortalDiagnostics::DescribeAdaptation() } };
			 } } },
			// Tests (run_suite's checks): see PortalDiagnostics::SceneCheck.
			{ "scene_check", { [](const json& a) { return PortalDiagnostics::SceneCheck(a.value("radius", 3000.0f)); } } },
			// Tests: late_pieces {outOfPlace: 0..1, settle: bool}: the merged room's pieces as a late load leaves them, and the settling (RemoteCell::SettleLatePieces) on or off.
			{ "late_pieces", { [](const json& a) {
				 if (a.contains("settle")) {
					 Spaces::RemoteCell::SetSettleLate(a.at("settle").get<bool>());
				 }
				 return a.contains("outOfPlace") ? PortalDiagnostics::PutPiecesOutOfPlace(a.at("outOfPlace").get<float>()) : json::object();
			 } } },
			{ "merged_shapes", { [](const json& a) {
				 const auto& p = a.at("near");
				 return json{ { "shapes", PortalDiagnostics::MergedShapesReport({ p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() }, a.value("far", 3000.0f)) } };
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
			// The tier scheduler. Testing without touching the ini: {loadedRooms} as [Portals] iLoadedRooms,
			// {memoryMb} as iHeldMemoryMB, {frameRate} as iTargetFrameRate (for the session). Returns the
			// scheduler's state, the rooms kept loaded, frame times and the merged cull.
			{ "schedule", { [](const json& a) {
				 if (a.contains("loadedRooms")) {
					 Portal::Schedule::SetLoadedRooms(a.at("loadedRooms").get<int>());
				 }
				 if (a.contains("memoryMb")) {
					 Portal::Schedule::SetMemoryBudgetMb(a.at("memoryMb").get<int>());
				 }
				 if (a.contains("frameRate")) {
					 Portal::Schedule::SetTargetFrameRate(a.at("frameRate").get<int>());
				 }
				 auto status = PortalDiagnostics::Status();
				 return json{ { "schedule", status["schedule"] }, { "warm", status["warm"] }, { "crossingPhase", status["crossingPhase"] }, { "mergedCull", status["mergedCull"] },
					 { "frames", FrameHooks::FrameStats() } };
			 } } },
			// The door graph built from the loaded game's teleport doors: counts by kind and link, how many
			// were there at data load and how many were found as cells loaded. {door: "<hex form id>"} adds
			// that door's record and the doorway it is part of.
			// Why a door shows its portal or not: {ref} or {plugin, id}. The scheduler's tier for the space behind it
			// and its last decision, the door's state and the first thing that stops it (`reason`; null while shown).
			{ "door_status", { [](const json& a) {
				 const auto* ref = RequireRef(a);
				 auto        status = Portal::Schedule::DescribeDoor(ref->GetFormID());
				 status["doorUse"] = Portal::DoorUse::Used(ref->GetFormID());
				 return status;
			 } } },
			// A form by plugin name and the ID within it ({plugin: "Skyrim.esm", id: "01A6F9"}), as the census and the
			// crawler name them; or by runtime form ID ({ref}) the other way round.
			{ "form_lookup", { [](const json& a) {
				 const auto id = a.contains("plugin") ? LookupInPlugin(a.at("plugin").get<std::string>(), ParseFormID(a.at("id"))) : ParseFormID(a.at("ref"));
				 auto*      form = RE::TESForm::LookupByID(id);
				 json       out{ { "formId", Hex(id) }, { "loaded", form != nullptr } };
				 if (form) {
					 const auto* file = form->GetFile(0);
					 out["plugin"] = file ? std::string(file->GetFilename()) : std::string();
					 out["localId"] = std::format("{:06X}", form->GetLocalFormID());
					 out["type"] = RE::FormTypeToString(form->GetFormType());
					 out["editorId"] = form->GetFormEditorID() ? form->GetFormEditorID() : "";
					 if (auto* ref = form->AsReference()) {
						 out["pos"] = Vec(ref->GetPosition());
						 out["cell"] = CellJson(ref->GetParentCell());
						 out["base"] = ref->GetBaseObject() ? Hex(ref->GetBaseObject()->GetFormID()) : "";
					 }
				 }
				 return out;
			 } } },
			{ "graph_summary", { [](const json& a) {
				 auto summary = Graph::Summary();
				 if (a.contains("door")) {
					 const auto  form = static_cast<RE::FormID>(std::stoul(a.at("door").get<std::string>(), nullptr, 16));
					 const auto  ix = Graph::DoorOf(form);
					 const auto& graph = Graph::Get();
					 if (ix != Core::Graph::kNoDoor && ix < graph.DoorCount()) {
						 const auto& door = graph.GetDoor(ix);
						 const auto& way = graph.GetDoorway(door.doorway);
						 summary["door"] = { { "id", door.id }, { "space", graph.GetSpace(door.space).id }, { "pos", { door.pos.x, door.pos.y, door.pos.z } },
							 { "link", way.link == Core::Graph::Link::Paired ? "paired" : way.link == Core::Graph::Link::OneWay ? "one-way" : "broken" },
							 { "kind", way.kind == Core::Graph::DoorKind::Gate ? "gate" : way.kind == Core::Graph::DoorKind::Hatch ? "hatch" : "hinged" },
							 { "leadsTo", way.Other(door.space) != Core::Graph::kNoSpace ? graph.GetSpace(way.Other(door.space)).id : "" } };
					 } else {
						 summary["door"] = nullptr;
					 }
				 }
				 return summary;
			 } } },
			// The doorway pictures on disk for this load order, per door: which tiers (image, depth image with
			// its views) and for which part of the day ("inside" for a room), where each came from (`visit`: the
			// player stood there; `ahead`: taken from the space held out of sight), its age in seconds, size and
			// whether it is in video memory now. {door: "<hex runtime id>"} or {plugin, id} limits it to one door.
			{ "pictures", { [](const json& a) {
				 std::string only;
				 if (a.contains("plugin") || a.contains("door")) {
					 const auto  id = a.contains("plugin") ? LookupInPlugin(a.at("plugin").get<std::string>(), ParseFormID(a.at("id"))) : ParseFormID(a.at("door"));
					 auto*       form = RE::TESForm::LookupByID(id);
					 auto*       file = form ? form->GetFile(0) : nullptr;
					 only = file ? Core::Pictures::DoorKey(file->GetFilename(), id, file->IsLight()) : Core::Pictures::DoorKeyWithoutPlugin(id);
				 }
				 const auto now = static_cast<std::uint32_t>(std::time(nullptr));
				 std::map<std::string, json> doors;
				 std::uint64_t total = 0;
				 for (const auto& listing : Pictures::Cache::List()) {
					 if (!only.empty() && listing.door != only) {
						 continue;
					 }
					 auto& door = doors[listing.door];
					 if (door.is_null()) {
						 door = { { "door", listing.door }, { "pictures", json::array() } };
					 }
					 total += listing.bytes;
					 door["pictures"].push_back({ { "tier", listing.layered ? "depth-image" : "image" }, { "part", listing.part }, { "views", listing.layered ? json(listing.layers) : json(nullptr) },
						 { "source", listing.source == Pictures::Cache::Source::kAhead ? "ahead" : "visit" }, { "ageSeconds", listing.takenAt ? json(now - listing.takenAt) : json(nullptr) },
						 { "bytes", listing.bytes }, { "inMemory", listing.inMemory } });
				 }
				 json out = json::array();
				 for (auto& [key, door] : doors) {
					 out.push_back(std::move(door));
				 }
				 return json{ { "doors", out }, { "files", Pictures::Cache::Count().inMemory + Pictures::Cache::Count().layered }, { "bytesOnDisk", total }, { "ahead", Portal::Ahead::Status() },
					 { "format", "BC6H colour with mips + R16F distance, version 7" } };
			 } } },
			// Takes the pictures of one of the player's space's doors from its space held out of sight, as the
			// scheduler's tiers ask for them: {ref} or {plugin, id}; {retake: true} drops what the door has for
			// this part of the day first. The space must be attached (the scheduler holds it at T3) or cheap to
			// attach for it (an interior of few references); it ignores the frame time. Watch `pictures`.
			{ "capture_ahead", { [](const json& a) {
				 const auto* ref = RequireRef(a, a.contains("door") ? "door" : "ref");
				 return Portal::Ahead::Force(ref->GetFormID(), a.value("retake", false));
			 } } },
			{ "portal_passes", { [](const json& a) { return PortalDiagnostics::RenderPasses(Portal::Manager::Space(), a.value("drop", false)); } } },
			{ "portals_enable", { [](const json& a) {
				 Portal::Manager::SetEnabled(a.value("enabled", true));
				 return PortalDiagnostics::Status();
			 } } },
			{ "portal_status", { [](const json&) {
				 auto status = PortalDiagnostics::Status();
				 status["lastFrame"] = ::Render::Frame::LastFrame();
				 status["travel"] = Portal::Travel::Status();
				 status["audio"] = Audio::Doorway::Status();
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
