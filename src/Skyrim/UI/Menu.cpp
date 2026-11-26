#include "Skyrim/UI/Menu.h"

#include "Skyrim/Audio/DoorwayAudio.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Plugin/Install.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Render/Native/Native.h"

#include "SKSEMenuFramework.h"

namespace
{
	using Plugin::Settings::Id;

	// What is set here is kept in the user ini (Settings::SaveUser), never in the shipped one.
	// A slider is written once, when it is let go: dragged, it changes every frame.
	void SaveWhenReleased(Id a_id, int a_value)
	{
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			Plugin::Settings::SaveUser(a_id, a_value);
		}
	}

	std::string MegabytesText(double a_bytes)
	{
		const double megabytes = a_bytes / 1e6;
		return megabytes < 10.0 ? std::format("{:.1f}", megabytes) : std::format("{:.0f}", megabytes);
	}

	// What one room door's picture takes on disk and in video memory at this doorway height.
	double RoomPictureBytes(int a_height)
	{
		const auto [width, height] = Pictures::Camera::SizeFor(Pictures::Camera::kRoomCamera.extent, a_height, Pictures::Camera::kNarrowest);
		return static_cast<double>(Core::Pictures::Compress::PictureBytes(width, height));
	}

	// What one view of a door's depth image takes on disk at this doorway height.
	double DepthViewBytes(int a_height)
	{
		const auto [width, height] = Pictures::Layers::SizeOf(a_height);
		return static_cast<double>(Core::Pictures::Compress::PictureBytes(width, height));
	}

	// How many times fewer texels a doorway-only picture (a door to the outdoors) has than a room's.
	long DoorwayOnlyShare(int a_height)
	{
		const auto [roomWidth, roomHeight] = Pictures::Camera::SizeFor(Pictures::Camera::kRoomCamera.extent, a_height, Pictures::Camera::kNarrowest);
		const auto [width, height] = Pictures::Camera::SizeFor(Pictures::Camera::kDoorwayOnly, a_height, Pictures::Camera::kNarrowest);
		return std::lround(static_cast<double>(roomWidth) * roomHeight / (static_cast<double>(width) * height));
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

	Pair CurrentPair()
	{
		Pair pair;
		if (const auto at = Portal::Selection::DoorPairAt(kNearDoorDistance)) {
			pair.source = RE::TESForm::LookupByID<RE::TESObjectREFR>(at->sourceDoor);
			pair.linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(at->linkedDoor);
			if (pair.source && pair.linked) {
				pair.key = Portal::Doors::CorrectionDoor(pair.source, pair.linked);
			}
		}
		return pair;
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
		const bool available = Plugin::HooksInstalled();

		if (available && !Render::Native::Ready()) {
			ImGui::TextWrapped("Rooms aren't drawn into the world: the renderer could not be hooked (see the plugin log).");
		}

		ImGui::NewLine();

		ImGui::BeginDisabled(!available);
		if (ImGui::Checkbox("Seamless doors", &enabled)) {
			Portal::Manager::SetEnabled(enabled);
		}
		ImGui::EndDisabled();
		if (!available) {
			ImGui::TextWrapped("Off for this session: set bEnabled=1 under [Portals] in %s and restart the game.", Plugin::Settings::kShippedFile);
		} else {
			// Not written to the ini: off there, the game starts without the mod's hooks, and this
			// box can't switch it on again.
			ImGui::TextWrapped("For this session; to keep it off, set bEnabled=0 in %s.", Plugin::Settings::kUserFile);
		}

		ImGui::NewLine();
		
		bool merge = Portal::MergedView::MergeEnabled();
		if (ImGui::Checkbox("Draw rooms into the world (seen from outside)", &merge)) {
			Portal::Manager::SetMergeEnabled(merge);
			Plugin::Settings::SaveUser(Id::kMerge, merge);
		}

		ImGui::NewLine();

		{
			const auto& loadedRange = Plugin::Settings::Of(Id::kLoadedRooms);
			int         loaded = Portal::Schedule::LoadedRooms();
			if (ImGui::SliderInt("Rooms kept loaded", &loaded, loadedRange.min, loadedRange.max)) {
				Portal::Schedule::SetLoadedRooms(loaded);
			}
			SaveWhenReleased(Id::kLoadedRooms, loaded);
			ImGui::TextWrapped("Besides the room at the door you stand at, the rooms behind the nearest other doors are kept loaded "
							   "out of sight, so a doorway opens at once when you walk up. Nothing in them runs until their door is open "
							   "and you are close. Each costs memory and a short frame when it loads.");

			ImGui::NewLine();

			const auto& memoryRange = Plugin::Settings::Of(Id::kMemoryBudgetMb);
			int         memory = Portal::Schedule::MemoryBudgetMb();
			if (ImGui::SliderInt("Memory for rooms (MB)", &memory, memoryRange.min, memoryRange.max)) {
				Portal::Schedule::SetMemoryBudgetMb(memory);
			}
			SaveWhenReleased(Id::kMemoryBudgetMb, memory);
			ImGui::TextWrapped("What the rooms kept loaded and the doorway pictures held for the doors around you may take together. "
							   "When it runs out, the door you are least likely to use next gives up its room, then its picture.");

			ImGui::NewLine();

			const auto& rateRange = Plugin::Settings::Of(Id::kTargetFrameRate);
			int         rate = Portal::Schedule::TargetFrameRate();
			if (ImGui::SliderInt("Frame rate to keep", &rate, rateRange.min, rateRange.max)) {
				Portal::Schedule::SetTargetFrameRate(rate);
			}
			SaveWhenReleased(Id::kTargetFrameRate, rate);
			ImGui::TextWrapped("A room behind a door is shown live only while the game has frame time to spare at this rate. "
							   "Below it, that door keeps to its picture and the game's own loading screen.");
		}

		ImGui::NewLine();

		bool step = Portal::Manager::StepExterior();
		if (ImGui::Checkbox("Street physics while indoors (people outside walk)", &step)) {
			Portal::Manager::SetStepExterior(step);
			Plugin::Settings::SaveUser(Id::kStepExteriorFromInside, step);
		}

		ImGui::NewLine();

		{
			bool sound = Audio::Doorway::Enabled();
			if (ImGui::Checkbox("Hear rooms through open doors", &sound)) {
				Audio::Doorway::SetEnabled(sound);
				Plugin::Settings::SaveUser(Id::kDoorwaySound, sound);
			}
			const auto& range = Plugin::Settings::Of(Id::kDoorwaySoundVolume);
			int         percent = Audio::Doorway::Volume();
			if (ImGui::SliderInt("Doorway sound volume (%)", &percent, range.min, range.max)) {
				Audio::Doorway::SetVolume(percent);
			}
			SaveWhenReleased(Id::kDoorwaySoundVolume, percent);
			ImGui::TextWrapped("Standing at an open door, the room behind it can be heard from the doorway, quieter and duller with "
							   "distance and angle, and gone again when the door shuts. Only the room that is live, only while its door is open.");
		}

		ImGui::NewLine();

		{
			int                   force = static_cast<int>(Portal::DoorUse::GetForce());
			constexpr const char* labels[]{ "As set for each door", "All open (locked ones too)", "All shut until used" };
			if (ImGui::Combo("Doors", &force, labels, static_cast<int>(std::size(labels)))) {
				Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(force));
				Plugin::Settings::SaveUser(Id::kForceDoors, force);
			}
			ImGui::TextWrapped("A door opens when you or someone else uses it, and shuts a few seconds after nobody stands in it. "
							   "City gates and doors set to always open stand open. All shut: those too stay shut until used.");
		}

		ImGui::NewLine();

		float doorDistance = Portal::Selection::DoorOpenDistance();
		const auto& reach = Plugin::Settings::Of(Id::kDoorOpenDistance);
		if (ImGui::SliderFloat("Door reach (game units)", &doorDistance, static_cast<float>(reach.min), static_cast<float>(reach.max), "%.0f")) {
			Portal::Selection::SetDoorOpenDistance(doorDistance);
		}
		SaveWhenReleased(Id::kDoorOpenDistance, static_cast<int>(std::lround(doorDistance)));
		ImGui::TextWrapped("The space behind a door starts loading %.0f units beyond this and is let go %.0f units beyond it.",
			Portal::Selection::kLoadAhead, Portal::Selection::kLoadAhead + Portal::Selection::kReleaseBeyond);

		ImGui::NewLine();

		{
			// Presets rather than a slider: each size is a texture and a file per door, so a few
			// steps that double are all that's worth offering.
			constexpr int         heights[]{ 64, 128, 192, 256, 384, 512, 768, 1024 };
			// The sizes on disk are a door into a room's: its picture reaches past the doorway
			// (Pictures::Camera).
			static const std::vector<std::string> labels = [&] {
				std::vector<std::string> texts;
				for (const int height : heights) {
					texts.push_back(std::format("{} x {} ({} MB per door{})", Pictures::Camera::WidthFor(height), height,
						MegabytesText(RoomPictureBytes(height)), height == Plugin::Settings::Of(Id::kDoorwayPictureHeight).def ? ", default" : ""));
				}
				return texts;
			}();
			const int current = Pictures::PictureHeight();
			// An ini value between the presets shows as the nearest one.
			int index = 0;
			for (int i = 0; i < static_cast<int>(std::size(heights)); ++i) {
				if (std::abs(heights[i] - current) < std::abs(heights[index] - current)) {
					index = i;
				}
			}
			std::vector<const char*> items;
			for (const auto& label : labels) {
				items.push_back(label.c_str());
			}
			if (ImGui::Combo("Doorway picture size (texels)", &index, items.data(), static_cast<int>(items.size()))) {
				Pictures::SetPictureHeight(heights[index]);
				Plugin::Settings::SaveUser(Id::kDoorwayPictureHeight, heights[index]);
			}
			ImGui::TextWrapped("The picture of the room behind a doorway, shown from a distance and while the room loads, in texels "
							   "across the doorway's height. Larger is sharper and takes more disk and video memory; on a 1080p screen "
							   "512 is about as sharp as it gets. A door is taken again at the new size when next seen open. A door to "
							   "the outdoors keeps one picture per time of day, of the doorway only (about %ld times smaller).",
				DoorwayOnlyShare(Plugin::Settings::Of(Id::kDoorwayPictureHeight).def));

			ImGui::NewLine();

			{
				constexpr const char* layerLabels[]{ "Off (the picture only)", "1 view", "2 views", "3 views" };
				const auto&           layersRange = Plugin::Settings::Of(Id::kDoorwayDepthLayers);
				int                   layers = std::clamp(Pictures::DepthLayers(), layersRange.min, layersRange.max);
				if (ImGui::Combo("Doorway depth views", &layers, layerLabels, static_cast<int>(std::size(layerLabels)))) {
					Plugin::Settings::SaveUser(Id::kDoorwayDepthLayers, layers);
				}
				ImGui::TextWrapped("Extra views of the room from just inside the doorway, used for doors closer than about a thousand "
								   "units: walking past, you see furniture shift against the wall behind it instead of a flat picture. "
								   "Each view is about %s MB per door at the picture size above. Not for city gates. Doors are taken "
								   "again when next seen open.",
					MegabytesText(DepthViewBytes(Pictures::PictureHeight())).c_str());
			}

			ImGui::NewLine();

			{
				const auto& cacheRange = Plugin::Settings::Of(Id::kDoorwayCacheMegabytes);
				int         cache = Plugin::Settings::Int(Id::kDoorwayCacheMegabytes);
				ImGui::SliderInt("Picture folder size (MB)", &cache, cacheRange.min, cacheRange.max, "%d", ImGuiSliderFlags_Logarithmic);
				SaveWhenReleased(Id::kDoorwayCacheMegabytes, cache);
				ImGui::TextWrapped("The most disk the doorway pictures may take in the SeamlessSkyrim_Doorways folder. Past it, the ones "
								   "you saw longest ago are deleted.");
			}

			ImGui::NewLine();

			// Until another click: the result of this one, so the click visibly did something.
			static std::optional<int> deleted;
			if (ImGui::Button("Delete all doorway pictures")) {
				deleted = Pictures::Cache::DeleteAll();
			}
			if (deleted) {
				ImGui::SameLine();
				ImGui::Text("%d deleted", *deleted);
			}
			ImGui::TextWrapped("Each door shows a plain colour until it is next seen open, and is taken again then.");
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
		if (Portal::Doors::Outdoors(pair.source) && Portal::Doors::Outdoors(pair.linked)) {
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
