#include "Skyrim/Plugin/Settings.h"

#include "Skyrim/Engine/Paths.h"

namespace Plugin::Settings
{
	namespace
	{
		struct Value
		{
			int          number = 0;
			std::wstring text;
		};
		std::array<Value, kEntries.size()> values;

		constexpr bool InOrder()
		{
			for (std::size_t i = 0; i < kEntries.size(); ++i) {
				if (static_cast<std::size_t>(kEntries[i].id) != i) {
					return false;
				}
			}
			return true;
		}
		static_assert(InOrder(), "kEntries must be in the order of Id");

		std::filesystem::path Shipped()
		{
			return Paths::PluginDirectory() / kShippedFile;
		}

		std::filesystem::path User()
		{
			return Paths::PluginDirectory() / kUserFile;
		}

		int Clamped(const Entry& a_entry, int a_value)
		{
			return a_entry.type == Type::kBool ? a_value != 0 : std::clamp(a_value, a_entry.min, a_entry.max);
		}
	}

	int RemoteHavokStepDefault()
	{
		return ::GetModuleHandleW(L"Precision.dll") != nullptr ? 2 : 1;
	}

	void Load()
	{
		const auto files = { Shipped(), User() };
		for (const auto& entry : kEntries) {
			auto& value = values[static_cast<std::size_t>(entry.id)];
			value.number = entry.computeDefault ? entry.computeDefault() : entry.def;
			value.text = entry.text;
			for (const auto& file : files) {
				if (entry.type == Type::kString) {
					wchar_t buffer[MAX_PATH]{};
					::GetPrivateProfileStringW(entry.section, entry.key, value.text.c_str(), buffer, MAX_PATH, file.c_str());
					value.text = buffer;
				} else {
					value.number = static_cast<int>(::GetPrivateProfileIntW(entry.section, entry.key, value.number, file.c_str()));
				}
			}
			if (entry.type != Type::kString) {
				// A negative value asks for the default of the install (as -1 always did).
				if (entry.computeDefault && value.number < entry.min) {
					value.number = entry.computeDefault();
				}
				value.number = Clamped(entry, value.number);
			}
		}
		logger::info("settings: {} then {}; remote Havok stepping mode {}", kShippedFile, kUserFile, Int(Id::kStepRemoteHavok));
	}

	int Int(Id a_id)
	{
		return values[static_cast<std::size_t>(a_id)].number;
	}

	bool Bool(Id a_id)
	{
		return Int(a_id) != 0;
	}

	const std::wstring& String(Id a_id)
	{
		return values[static_cast<std::size_t>(a_id)].text;
	}

	void SaveUser(Id a_id, int a_value)
	{
		const auto& entry = Of(a_id);
		const int   value = Clamped(entry, a_value);
		values[static_cast<std::size_t>(a_id)].number = value;
		::WritePrivateProfileStringW(entry.section, entry.key, std::to_wstring(value).c_str(), User().c_str());
	}
}
