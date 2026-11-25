#pragma once

// What the command files share: reading arguments and describing engine objects as JSON.
namespace Bridge::Support
{
	std::string Hex(RE::FormID a_id);
	// A pointer as hex, for diagnostics.
	std::string Address(const void* a_pointer);
	// Accepts 0x16072, "0x16072", "16072" or "0001605E": form IDs are hex everywhere in modding.
	RE::FormID ParseFormID(const json& a_value);
	// A form named by plugin and the ID within it ({"plugin": "Skyrim.esm", "id": "01A6F9"}, the
	// door census's and the crawler's names), as the runtime form ID; throws if the plugin isn't loaded
	// or has no such form.
	RE::FormID LookupInPlugin(std::string_view a_plugin, RE::FormID a_localId);
	// The reference an argument names: {"ref": id} (key can differ), or {"plugin", "id"}.
	RE::TESObjectREFR* RequireRef(const json& a_args, const char* a_key = "ref");
	RE::PlayerCharacter* RequirePlayer();
	json Vec(const RE::NiPoint3& a_p);
	json CellJson(RE::TESObjectCELL* a_cell);
}
