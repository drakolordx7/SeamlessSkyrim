#include "Skyrim/Bridge/CommandSupport.h"

#include "Skyrim/Diag/Probe.h"

namespace Bridge::Support
{
	std::string Hex(RE::FormID a_id)
	{
		return std::format("{:08X}", a_id);
	}

	RE::FormID ParseFormID(const json& a_value)
	{
		if (a_value.is_number_unsigned() || a_value.is_number_integer()) {
			return a_value.get<RE::FormID>();
		}
		auto text = a_value.get<std::string>();
		if (text.starts_with("0x") || text.starts_with("0X")) {
			text.erase(0, 2);
		}
		return static_cast<RE::FormID>(std::stoul(text, nullptr, 16));
	}

	RE::TESObjectREFR* RequireRef(const json& a_args, const char* a_key)
	{
		const auto id = ParseFormID(a_args.at(a_key));
		auto*      ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
		if (!ref) {
			throw std::runtime_error(std::format("{:08X} is not a loaded reference", id));
		}
		return ref;
	}

	RE::PlayerCharacter* RequirePlayer()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			throw std::runtime_error("no player in the world (main menu or loading?)");
		}
		return player;
	}

	json Vec(const RE::NiPoint3& a_p)
	{
		return { a_p.x, a_p.y, a_p.z };
	}

	json CellJson(RE::TESObjectCELL* a_cell)
	{
		if (!a_cell) {
			return nullptr;
		}
		const char* edid = a_cell->GetFormEditorID();
		return {
			{ "id", Hex(a_cell->GetFormID()) },
			{ "editorId", edid ? edid : "" },
			{ "name", a_cell->GetFullName() },
			{ "interior", a_cell->IsInteriorCell() },
			{ "summary", Probe::DescribeCell(a_cell) },
		};
	}
}
