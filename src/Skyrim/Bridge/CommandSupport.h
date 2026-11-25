#pragma once

// What the command files share: reading arguments and describing engine objects as JSON.
namespace Bridge::Support
{
	std::string Hex(RE::FormID a_id);
	// A pointer as hex, for diagnostics.
	std::string Address(const void* a_pointer);
	// Accepts 0x16072, "0x16072", "16072" or "0001605E": form IDs are hex everywhere in modding.
	RE::FormID ParseFormID(const json& a_value);
	RE::TESObjectREFR* RequireRef(const json& a_args, const char* a_key = "ref");
	RE::PlayerCharacter* RequirePlayer();
	json Vec(const RE::NiPoint3& a_p);
	json CellJson(RE::TESObjectCELL* a_cell);
}
