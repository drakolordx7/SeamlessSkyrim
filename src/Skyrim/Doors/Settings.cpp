#include "Skyrim/Doors/Settings.h"

#include "Skyrim/Doors/Door.h"
#include "Skyrim/Engine/Paths.h"

namespace Portal::Doors
{
	namespace
	{
		std::mutex                            correctionsLock;
		std::unordered_map<RE::FormID, float> corrections;  // under correctionsLock
		std::unordered_map<RE::FormID, float> swingDegrees;  // under correctionsLock
		std::unordered_set<RE::FormID>        portalsOff;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        opensInward;  // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        alwaysOpen;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        farPicture;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_map<RE::FormID, RE::NiPoint3> shifts;  // under correctionsLock
		bool                                  correctionsLoaded = false;

		// The doors the mod ships with. Never written: an update replaces it, which would take the
		// player's own tunings with it if they were kept here too.
		std::filesystem::path DefaultsFile()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doors.ini";
		}

		// The doors tuned in game. No release ships it, so no update replaces it.
		std::filesystem::path TuningsFile()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doors_User.ini";
		}

		// "Skyrim.esm|01A6F5": the same door whatever the load order.
		std::string CorrectionKey(RE::FormID a_door)
		{
			auto* form = RE::TESForm::LookupByID(a_door);
			auto* file = form ? form->GetFile(0) : nullptr;
			if (!file) {
				return std::format("{:08X}", a_door);
			}
			const auto local = file->IsLight() ? (a_door & 0xFFF) : (a_door & 0xFFFFFF);
			return std::format("{}|{:06X}", file->GetFilename(), local);
		}

		// The file's lines by section, read by hand: the system's own reader (GetPrivateProfileSection)
		// gives up to 32767 characters of a section and drops the rest without a word, the last
		// entry cut in the middle. A patcher's list for a whole load order is longer than that.
		using Sections = std::vector<std::pair<std::wstring, std::vector<std::wstring>>>;
		Sections ReadSections(const std::filesystem::path& a_file)
		{
			std::ifstream     stream(a_file, std::ios::binary);
			const std::string bytes{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
			std::wstring      text;
			if (bytes.starts_with("\xFF\xFE")) {
				text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
			} else {
				// As the system's writer (WritePrivateProfileString, SaveCorrection) leaves it.
				text.resize(bytes.size());
				text.resize(::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), static_cast<int>(text.size())));
			}
			const auto trimmed = [](std::wstring_view a_text) {
				const auto first = a_text.find_first_not_of(L" \t\r");
				const auto last = a_text.find_last_not_of(L" \t\r");
				return first == std::wstring_view::npos ? std::wstring_view{} : a_text.substr(first, last - first + 1);
			};
			Sections sections;
			for (std::size_t at = 0; at < text.size();) {
				const auto end = std::min(text.find(L'\n', at), text.size());
				const auto line = trimmed(std::wstring_view(text).substr(at, end - at));
				at = end + 1;
				if (line.empty() || line.front() == L';') {
					continue;
				}
				if (line.front() == L'[' && line.back() == L']') {
					sections.emplace_back(line.substr(1, line.size() - 2), std::vector<std::wstring>{});
				} else if (!sections.empty()) {
					sections.back().second.emplace_back(line);
				}
			}
			return sections;
		}

		// Every section's entries are Plugin.esp|localID=value; a_use gets the door and the value. An
		// empty a_section: the entries of every section.
		void ForEachEntry(const Sections& a_sections, std::wstring_view a_section, const std::function<void(RE::FormID, const wchar_t*)>& a_use)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (const auto& [name, entries] : a_sections) {
				if (!a_section.empty() && ::_wcsnicmp(name.c_str(), a_section.data(), std::max(name.size(), a_section.size())) != 0) {
					continue;
				}
				for (const auto& entry : entries) {
					const auto equals = entry.find(L'=');
					const auto bar = entry.find(L'|');
					if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals) {
						continue;
					}
					const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
					const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
					if (const auto id = handler->LookupFormID(local, file)) {
						a_use(id, entry.c_str() + equals + 1);
					}
				}
			}
		}

		// Under correctionsLock.
		void Apply(const Sections& a_sections)
		{
			const auto each = [&](std::wstring_view a_section, const std::function<void(RE::FormID, const wchar_t*)>& a_use) {
				ForEachEntry(a_sections, a_section, a_use);
			};
			each(L"DoorTurn", [](RE::FormID a_door, const wchar_t* a_value) {
				if (const float degrees = std::wcstof(a_value, nullptr); degrees != 0.0f) {
					corrections[a_door] = degrees;
				}
			});
			each(L"DoorSwing", [](RE::FormID a_door, const wchar_t* a_value) { swingDegrees[a_door] = std::wcstof(a_value, nullptr); });
			// [DoorPortal] =0: the pair's portal switched off.
			each(L"DoorPortal", [](RE::FormID a_door, const wchar_t* a_value) {
				if (std::wcstol(a_value, nullptr, 10) == 0) {
					portalsOff.insert(a_door);
				}
			});
			// [DoorOpensIn], [DoorAlwaysOpen] and [DoorFarPicture] =1.
			for (const auto& [section, set] : { std::pair{ L"DoorOpensIn", &opensInward }, std::pair{ L"DoorAlwaysOpen", &alwaysOpen }, std::pair{ L"DoorFarPicture", &farPicture } }) {
				each(section, [set](RE::FormID a_door, const wchar_t* a_value) {
					if (std::wcstol(a_value, nullptr, 10) != 0) {
						set->insert(a_door);
					}
				});
			}
			// [DoorOffset] =x,y,z
			each(L"DoorOffset", [](RE::FormID a_door, const wchar_t* a_value) {
				RE::NiPoint3 shift;
				if (std::swscanf(a_value, L"%f,%f,%f", &shift.x, &shift.y, &shift.z) == 3) {
					shifts[a_door] = shift;
				}
			});
		}

		void LoadCorrections()
		{
			if (std::exchange(correctionsLoaded, true)) {
				return;
			}
			Apply(ReadSections(DefaultsFile()));
			// A door the player tuned takes none of the shipped settings: an entry missing from the
			// tunings means the built-in default (a portal switched back on, a door no longer shifted),
			// not the shipped value. SaveCorrection always writes [DoorPortal], so a tuned door is in
			// the file even when all its settings are the built-in ones.
			const auto              tunings = ReadSections(TuningsFile());
			std::vector<RE::FormID> tuned;
			ForEachEntry(tunings, {}, [&](RE::FormID a_door, const wchar_t*) {
				if (std::ranges::find(tuned, a_door) == tuned.end()) {
					tuned.push_back(a_door);
				}
			});
			for (const auto door : tuned) {
				corrections.erase(door);
				swingDegrees.erase(door);
				portalsOff.erase(door);
				opensInward.erase(door);
				alwaysOpen.erase(door);
				farPicture.erase(door);
				shifts.erase(door);
			}
			Apply(tunings);
			logger::info("portal: {} door turn(s), {} door shift(s), {} swing angle(s), {} portal(s) off, {} opening inward, {} always open, {} gate(s) with a picture from afar loaded; {} door(s) tuned by the player",
				corrections.size(), shifts.size(), swingDegrees.size(), portalsOff.size(), opensInward.size(), alwaysOpen.size(), farPicture.size(), tuned.size());
		}

		bool Has(const std::unordered_set<RE::FormID>& a_doors, RE::FormID a_door)
		{
			std::scoped_lock lock(correctionsLock);
			LoadCorrections();
			return a_doors.contains(a_door);
		}

		void Set(std::unordered_set<RE::FormID>& a_doors, RE::FormID a_door, bool a_on)
		{
			std::scoped_lock lock(correctionsLock);
			LoadCorrections();
			if (a_on) {
				a_doors.insert(a_door);
			} else {
				a_doors.erase(a_door);
			}
		}
	}

	RE::TESObjectREFR* CorrectionDoor(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked)
	{
		return CorrectionDoor(Side(a_source), Side(a_linked)) == a_source->GetFormID() ? a_source : a_linked;
	}

	RE::FormID CorrectionDoor(const Side& a_source, const Side& a_linked)
	{
		if (a_source.outdoors != a_linked.outdoors) {
			return a_source.outdoors ? a_source.id : a_linked.id;
		}
		return a_source.id < a_linked.id ? a_source.id : a_linked.id;
	}

	RE::FormID SwingingDoor(const Side& a_source, const Side& a_linked)
	{
		const auto key = CorrectionDoor(a_source, a_linked);
		return OpensInward(key) == (key == a_source.id) ? a_linked.id : a_source.id;
	}

	float Correction(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = corrections.find(a_door);
		return it != corrections.end() ? it->second : 0.0f;
	}

	void SetCorrection(RE::FormID a_door, float a_degrees)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		corrections[a_door] = a_degrees;
	}

	void SaveCorrection(RE::FormID a_door)
	{
		const float degrees = Correction(a_door);
		const auto  key = CorrectionKey(a_door);
		const std::wstring wkey = Paths::Widen(key);
		const auto         file = TuningsFile();
		const auto         value = std::format(L"{:.2f}", degrees);
		::WritePrivateProfileStringW(L"DoorTurn", wkey.c_str(), degrees != 0.0f ? value.c_str() : nullptr, file.c_str());
		const float swing = SwingDegrees(a_door);
		const auto  swingValue = std::format(L"{:.0f}", swing);
		::WritePrivateProfileStringW(L"DoorSwing", wkey.c_str(), swing != kSwingDegrees ? swingValue.c_str() : nullptr, file.c_str());
		const auto shift = Shift(a_door);
		const auto shiftValue = std::format(L"{:.1f},{:.1f},{:.1f}", shift.x, shift.y, shift.z);
		const bool shifted = shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f;
		::WritePrivateProfileStringW(L"DoorOffset", wkey.c_str(), shifted ? shiftValue.c_str() : nullptr, file.c_str());
		const bool off = PortalOff(a_door);
		const bool inward = OpensInward(a_door);
		const bool open = AlwaysOpen(a_door);
		const bool picture = PictureFromAfar(a_door);
		// Written even when on: it keeps the door in the file, so LoadCorrections lets none of the
		// shipped settings back in.
		::WritePrivateProfileStringW(L"DoorPortal", wkey.c_str(), off ? L"0" : L"1", file.c_str());
		::WritePrivateProfileStringW(L"DoorOpensIn", wkey.c_str(), inward ? L"1" : nullptr, file.c_str());
		::WritePrivateProfileStringW(L"DoorAlwaysOpen", wkey.c_str(), open ? L"1" : nullptr, file.c_str());
		::WritePrivateProfileStringW(L"DoorFarPicture", wkey.c_str(), picture ? L"1" : nullptr, file.c_str());
		logger::info("portal: door {} saved: turned {:.2f} degrees, opens by {:.0f} degrees{}{}{}{}", key, degrees, swing,
			off ? ", portal off" : ", portal on", inward ? ", opens inward" : "", open ? ", always open" : "", picture ? ", picture from afar" : "");
	}

	RE::NiPoint3 Shift(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = shifts.find(a_door);
		return it != shifts.end() ? it->second : RE::NiPoint3{};
	}

	void SetShift(RE::FormID a_door, const RE::NiPoint3& a_shift)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		shifts[a_door] = a_shift;
	}

	bool PortalOff(RE::FormID a_door)
	{
		return Has(portalsOff, a_door);
	}

	void SetPortalOff(RE::FormID a_door, bool a_off)
	{
		Set(portalsOff, a_door, a_off);
	}

	bool OpensInward(RE::FormID a_door)
	{
		return Has(opensInward, a_door);
	}

	void SetOpensInward(RE::FormID a_door, bool a_inward)
	{
		Set(opensInward, a_door, a_inward);
	}

	bool AlwaysOpen(RE::FormID a_door)
	{
		return Has(alwaysOpen, a_door);
	}

	void SetAlwaysOpen(RE::FormID a_door, bool a_open)
	{
		Set(alwaysOpen, a_door, a_open);
	}

	std::vector<RE::FormID> AlwaysOpenDoors()
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return { alwaysOpen.begin(), alwaysOpen.end() };
	}

	bool PictureFromAfar(RE::FormID a_door)
	{
		return Has(farPicture, a_door);
	}

	void SetPictureFromAfar(RE::FormID a_door, bool a_picture)
	{
		Set(farPicture, a_door, a_picture);
	}

	float SwingDegrees(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = swingDegrees.find(a_door);
		return it != swingDegrees.end() ? it->second : kSwingDegrees;
	}

	void SetSwingDegrees(RE::FormID a_door, float a_degrees)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		swingDegrees[a_door] = a_degrees;
	}
}
