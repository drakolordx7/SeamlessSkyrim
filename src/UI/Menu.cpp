#include "UI/Menu.h"

#include "Doors/DoorPair.h"
#include "Doors/DoorUse.h"
#include "Pictures/DoorwaySnapshot.h"
#include "Portal/PortalManager.h"
#include "Render/Backend.h"

#include "Engine/Paths.h"
#include "SKSEMenuFramework.h"

namespace
{
	// What is set here is written to SeamlessSkyrim.ini, as the ini's comments say of these keys
	// ("Also in the in-game menu"): kept for the session only, every setting was back at the next
	// start of the game.
	void Save(const wchar_t* a_key, int a_value)
	{
		const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
		::WritePrivateProfileStringW(L"Portals", a_key, std::to_wstring(a_value).c_str(), ini.c_str());
	}

	// A slider is written once, when it is let go: dragged, it changes every frame.
	void SaveWhenReleased(const wchar_t* a_key, int a_value)
	{
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			Save(a_key, a_value);
		}
	}

	// The door pair of the doorway the player is at, for the correction controls.
	struct Pair
	{
		RE::TESObjectREFR* source = nullptr;
		RE::TESObjectREFR* linked = nullptr;
		RE::TESObjectREFR* key = nullptr;  // the door the correction is stored under
	};

	// The nearest door that could be a portal, within this. A door switched off here never opens a
	// portal again, so this is the only way back to its settings.
	constexpr float kNearDoorDistance = 600.0f;

	Pair MakePair(RE::FormID a_source, RE::FormID a_linked)
	{
		Pair pair;
		pair.source = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_source);
		pair.linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_linked);
		if (pair.source && pair.linked) {
			pair.key = Portal::Doors::CorrectionDoor(pair.source, pair.linked);
		}
		return pair;
	}

	Pair CurrentPair()
	{
		if (const auto at = Portal::Manager::DoorPairAt(kNearDoorDistance)) {
			return MakePair(at->sourceDoor, at->linkedDoor);
		}
		return {};
	}

	bool Outdoors(RE::TESObjectREFR* a_door)
	{
		auto* cell = a_door->GetParentCell();
		return cell ? !cell->IsInteriorCell() : a_door->GetWorldspace() != nullptr;
	}

	std::string Describe(RE::TESObjectREFR* a_door)
	{
		auto* cell = a_door ? a_door->GetParentCell() : nullptr;
		const char* name = cell && cell->GetFullName() && *cell->GetFullName() ? cell->GetFullName() : (cell ? cell->GetFormEditorID() : "");
		return a_door ? std::format("{:08X} ({})", a_door->GetFormID(), name ? name : "") : "-";
	}

	void __stdcall RenderSettings()
	{
		bool enabled = Portal::Manager::Enabled();
		const bool available = Portal::Manager::HooksInstalled();

		ImGui::TextWrapped("Rendering: %s", Render::Backend::Name());
		if (available && !Render::Backend::CanMerge()) {
			ImGui::TextWrapped("Rooms aren't drawn into the world with this renderer (see [Render] sBackend in SeamlessSkyrim.ini).");
		}

		ImGui::NewLine();

		ImGui::BeginDisabled(!available);
		if (ImGui::Checkbox("Seamless doors", &enabled)) {
			Portal::Manager::SetEnabled(enabled);
		}
		ImGui::EndDisabled();
		if (!available) {
			ImGui::TextWrapped("Off for this session: set bEnabled=1 under [Portals] in SeamlessSkyrim.ini and restart the game.");
		} else {
			// Not written to the ini: off there, the game starts without the mod's hooks, and this
			// box can't switch it on again.
			ImGui::TextWrapped("For this session; to keep it off, set bEnabled=0 in SeamlessSkyrim.ini.");
		}

		ImGui::NewLine();
		
		bool merge = Portal::Manager::MergeEnabled();
		if (ImGui::Checkbox("Draw rooms into the world (seen from outside)", &merge)) {
			Portal::Manager::SetMergeEnabled(merge);
			Save(L"bMerge", merge);
		}

		ImGui::NewLine();

		int shown = Portal::Manager::ShownRooms();
		if (ImGui::SliderInt("Rooms active at once", &shown, 1, Portal::Manager::kMaxShownRooms)) {
			Portal::Manager::SetShownRooms(shown);
		}
		SaveWhenReleased(L"iShownRooms", shown);
		ImGui::TextWrapped("Outdoors, the rooms behind the nearest doors are loaded, so a doorway opens at once when you walk up, "
						   "and each open door shows its own room. Counts the room you're at; 1 = only that one. Each costs "
						   "memory, a short frame when it loads, frame time while its doorway is on screen, and its people "
						   "are simulated.");
		if (Render::Backend::MergedRooms() < 2) {
			ImGui::TextWrapped("This renderer draws one room at a time: the others stay loaded out of sight, and their open doors "
							   "show a picture.");
		}

		ImGui::NewLine();

		bool pause = Portal::Manager::PauseUnseenRooms();
		if (ImGui::Checkbox("Pause rooms nobody can see", &pause)) {
			Portal::Manager::SetPauseUnseenRooms(pause);
			Save(L"bPauseUnseenRooms", pause);
		}
		ImGui::TextWrapped("A loaded room whose doorway is out of view isn't simulated or drawn until it comes into view; "
						   "the room at the door you're at always runs.");

		ImGui::NewLine();

		bool step = Portal::Manager::StepExterior();
		if (ImGui::Checkbox("Street physics while indoors (people outside walk)", &step)) {
			Portal::Manager::SetStepExterior(step);
			Save(L"bStepExteriorFromInside", step);
		}

		ImGui::NewLine();

		{
			int                   force = static_cast<int>(Portal::DoorUse::GetForce());
			constexpr const char* labels[]{ "As set for each door", "All open (locked ones too)", "All shut until used" };
			if (ImGui::Combo("Doors", &force, labels, static_cast<int>(std::size(labels)))) {
				Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(force));
				Save(L"iForceDoors", force);
			}
			ImGui::TextWrapped("A door opens when you or someone else uses it, and shuts a few seconds after nobody stands in it. "
							   "City gates and doors set to always open stand open. All shut: those too stay shut until used.");
		}

		ImGui::NewLine();

		float doorDistance = Portal::Manager::DoorOpenDistance();
		if (ImGui::SliderFloat("Door reach (game units)", &doorDistance, 150.0f, 1000.0f, "%.0f")) {
			Portal::Manager::SetDoorOpenDistance(doorDistance);
		}
		SaveWhenReleased(L"iDoorOpenDistance", static_cast<int>(std::lround(doorDistance)));
		ImGui::TextWrapped("The space behind a door starts loading 300 units beyond this and is let go 700 units beyond it.");

		ImGui::NewLine();

		{
			// Presets rather than a slider: each size is a texture and a file per door, so a few
			// steps that double are all that's worth offering.
			constexpr int         heights[]{ 64, 128, 192, 256, 384, 512 };
			// The sizes on disk are a door into a room's: its picture reaches past the doorway (3.5
			// times as wide, 1.75 times as high) at twice the density (Snapshot's kCameraExtent and
			// kCameraDensity).
			constexpr const char* labels[]{ "48 x 64 (0.6 MB per door)", "96 x 128 (2.4 MB per door, default)", "144 x 192 (5.4 MB per door)",
				"192 x 256 (9.6 MB per door)", "288 x 384 (22 MB per door)", "384 x 512 (39 MB per door)" };
			const int current = Portal::Snapshot::PictureHeight();
			// An ini value between the presets shows as the nearest one.
			int index = 0;
			for (int i = 0; i < static_cast<int>(std::size(heights)); ++i) {
				if (std::abs(heights[i] - current) < std::abs(heights[index] - current)) {
					index = i;
				}
			}
			if (ImGui::Combo("Doorway picture size (texels)", &index, labels, static_cast<int>(std::size(labels)))) {
				Portal::Snapshot::SetPictureHeight(heights[index]);
				Save(L"iDoorwayPictureHeight", heights[index]);
			}
			ImGui::TextWrapped("The blurred picture a doorway shows while the space behind it loads, in texels across the doorway. "
							   "Larger is sharper; each door is taken again at the new size when next seen open (doors to the outdoors "
							   "keep one per part of the day, of the doorway only: a 25th of the size).");
			// Until another click: the result of this one, so the click visibly did something.
			static std::optional<int> deleted;
			if (ImGui::Button("Delete all doorway pictures")) {
				deleted = Portal::Snapshot::DeleteAll();
			}
			if (deleted) {
				ImGui::SameLine();
				ImGui::Text("%d deleted", *deleted);
			}
			ImGui::TextWrapped("Each door shows the plain colour until it is next seen open, and is taken again then.");
		}
	}

	void __stdcall RenderDoor()
	{
		const auto pair = CurrentPair();
		if (!pair.key) {
			ImGui::TextWrapped("Walk up to a door: its doorway's settings appear here.");
			return;
		}
		ImGui::Text("Door: %s", Describe(pair.source).c_str());
		ImGui::Text("Leads to: %s", Describe(pair.linked).c_str());

		ImGui::NewLine();

		if (ImGui::Button("Save door settings")) {
			Portal::Doors::SaveCorrection(pair.key->GetFormID());
		}

		ImGui::NewLine();

		bool portal = !Portal::Doors::PortalOff(pair.key->GetFormID());
		if (ImGui::Checkbox("Seamless door (off: an ordinary door with a loading screen)", &portal)) {
			Portal::Doors::SetPortalOff(pair.key->GetFormID(), !portal);
		}
		bool inward = Portal::Doors::OpensInward(pair.key->GetFormID());
		if (ImGui::Checkbox("Opens inward (off: outward, into the street)", &inward)) {
			Portal::Doors::SetOpensInward(pair.key->GetFormID(), inward);
		}
		bool always = Portal::Doors::AlwaysOpen(pair.key->GetFormID());
		if (ImGui::Checkbox("Always open (not only when used; once its doorway has a picture)", &always)) {
			Portal::Doors::SetAlwaysOpen(pair.key->GetFormID(), always);
		}
		if (Outdoors(pair.source) && Outdoors(pair.linked)) {
			bool picture = Portal::Doors::PictureFromAfar(pair.key->GetFormID());
			if (ImGui::Checkbox("Plain colour until it has a picture (a gate walled up behind)", &picture)) {
				Portal::Doors::SetPictureFromAfar(pair.key->GetFormID(), picture);
			}
		}
		
		ImGui::NewLine();

		ImGui::TextWrapped("If the room behind the doorway doesn't line up with the outside, turn it here. It follows the slider "
						   "live.");
		float degrees = Portal::Doors::Correction(pair.key->GetFormID());
		if (ImGui::SliderFloat("Turn the inside (degrees)", &degrees, -15.0f, 15.0f, "%.1f")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees);
		}
		if (ImGui::Button("-0.1")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees - 0.1f);
		}
		ImGui::SameLine();
		if (ImGui::Button("+0.1")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), degrees + 0.1f);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			Portal::Doors::SetCorrection(pair.key->GetFormID(), 0.0f);
		}

		ImGui::NewLine();

		ImGui::TextWrapped("Move the inside against the outside (game units; sideways and further in as seen walking in):");
		auto shift = Portal::Doors::Shift(pair.key->GetFormID());
		bool moved = ImGui::SliderFloat("Sideways", &shift.x, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat("Further in", &shift.y, -60.0f, 60.0f, "%.1f");
		moved |= ImGui::SliderFloat("Up", &shift.z, -40.0f, 40.0f, "%.1f");
		if (moved) {
			Portal::Doors::SetShift(pair.key->GetFormID(), shift);
		}
		if (ImGui::Button("Reset position")) {
			Portal::Doors::SetShift(pair.key->GetFormID(), {});
		}
		
		ImGui::NewLine();

		float swing = Portal::Doors::SwingDegrees(pair.key->GetFormID());
		if (ImGui::SliderFloat("How far it opens (degrees)", &swing, 30.0f, 120.0f, "%.0f")) {
			Portal::Doors::SetSwingDegrees(pair.key->GetFormID(), swing);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##swing")) {
			Portal::Doors::SetSwingDegrees(pair.key->GetFormID(), Portal::Doors::kSwingDegrees);
		}
	}
}

namespace UI::Menu
{
	void Register()
	{
		// The framework's header looks the DLL up while ours loads, which can be before it (plugins
		// load in name order): looked up again now that every plugin is loaded.
		menuFramework = ::GetModuleHandleW(L"SKSEMenuFramework");
		if (!menuFramework) {
			logger::info("menu: SKSE Menu Framework not installed; no in-game settings");
			return;
		}
		SKSEMenuFramework::SetSection("Seamless Skyrim");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddSectionItem("This doorway", RenderDoor);
		logger::info("menu: registered with SKSE Menu Framework");
	}
}
