#include "Skyrim/Bridge/ReverseEngineering.h"

#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/PortalManager.h"

#include <REL/Offset2ID.h>

#include <Windows.h>

#include <fstream>

namespace ReverseEngineering
{
	json ExportIDs(const json& a_args)
	{
		const std::filesystem::path path = a_args.at("path").get<std::string>();
		const REL::Offset2ID        table;  // sorted by offset

		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			throw std::runtime_error(std::format("cannot write {}", path.string()));
		}
		out << "id,offset\n";
		for (const auto& entry : table) {
			out << entry.id << ",0x" << std::hex << entry.offset << std::dec << "\n";
		}
		return {
			{ "path", path.string() },
			{ "count", table.size() },
			{ "runtime", REL::Module::get().version().string() },
		};
	}

	json ReadMemory(const json& a_args)
	{
		constexpr std::size_t kMaxRead = 1 << 20;
		const auto            offset = a_args.at("offset").get<std::uint64_t>();
		const auto            size = std::min<std::size_t>(a_args.at("size").get<std::size_t>(), kMaxRead);

		// ReadProcessMemory on ourselves instead of a plain memcpy: it fails cleanly on unmapped or
		// guard pages, where a memcpy would crash the game.
		std::vector<std::uint8_t> buffer(size);
		SIZE_T                    read = 0;
		const auto                address = reinterpret_cast<const void*>(REL::Module::get().base() + offset);
		if (!ReadProcessMemory(GetCurrentProcess(), address, buffer.data(), size, &read) && read == 0) {
			throw std::runtime_error(std::format("cannot read 0x{:X} bytes at offset 0x{:X}", size, offset));
		}

		static constexpr char kHex[] = "0123456789abcdef";
		std::string           hex;
		hex.reserve(read * 2);
		for (std::size_t i = 0; i < read; ++i) {
			hex.push_back(kHex[buffer[i] >> 4]);
			hex.push_back(kHex[buffer[i] & 0xF]);
		}
		return { { "offset", offset }, { "size", read }, { "hex", hex } };
	}

	namespace
	{
		RE::FormID FormIDArg(const json& a_value)
		{
			if (a_value.is_string()) {
				auto text = a_value.get<std::string>();
				if (text.starts_with("0x")) {
					text.erase(0, 2);
				}
				return static_cast<RE::FormID>(std::stoul(text, nullptr, 16));
			}
			return a_value.get<RE::FormID>();
		}

		// Argument descriptors, so the agent can name engine objects instead of raw pointers:
		//   {"cell": "portal" | formid}, {"ref": formid}, {"node": "portalRoot"}, {"tes": true},
		//   {"player": true}, {"singleton": id} (pointer stored at an Address Library id),
		//   {"int": n}, {"bool": b}
		std::uintptr_t ResolveArg(const json& a_arg)
		{
			if (a_arg.contains("int")) {
				return static_cast<std::uintptr_t>(a_arg["int"].get<std::int64_t>());
			}
			if (a_arg.contains("bool")) {
				return a_arg["bool"].get<bool>() ? 1 : 0;
			}
			if (a_arg.contains("tes")) {
				return reinterpret_cast<std::uintptr_t>(RE::TES::GetSingleton());
			}
			if (a_arg.contains("player")) {
				return reinterpret_cast<std::uintptr_t>(RE::PlayerCharacter::GetSingleton());
			}
			if (a_arg.contains("node")) {
				return reinterpret_cast<std::uintptr_t>(Portal::LightScene::Get());
			}
			if (a_arg.contains("singleton")) {
				REL::Relocation<std::uintptr_t*> slot{ REL::ID(a_arg["singleton"].get<std::uint64_t>()) };
				return *slot;
			}
			if (a_arg.contains("cell")) {
				const auto& v = a_arg["cell"];
				auto* cell = v.is_string() && v.get<std::string>() == "portal" ? (Portal::Manager::ActivePortal() ? Portal::Manager::ActivePortal()->cell : nullptr) : RE::TESForm::LookupByID<RE::TESObjectCELL>(FormIDArg(v));
				if (!cell) {
					throw std::runtime_error("cell not found");
				}
				return reinterpret_cast<std::uintptr_t>(cell);
			}
			if (a_arg.contains("ref")) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(FormIDArg(a_arg["ref"]));
				if (!ref) {
					throw std::runtime_error("reference not found");
				}
				return reinterpret_cast<std::uintptr_t>(ref);
			}
			throw std::runtime_error(std::format("unknown argument descriptor {}", a_arg.dump()));
		}
	}

	json CallFunction(const json& a_args)
	{
		const auto id = a_args.at("id").get<std::uint64_t>();
		const auto args = a_args.value("args", json::array());
		if (args.size() > 4) {
			throw std::runtime_error("at most 4 arguments (register-passed only)");
		}
		std::uintptr_t values[4]{};
		for (std::size_t i = 0; i < args.size(); ++i) {
			values[i] = ResolveArg(args[i]);
		}
		// All-integer signature: correct for pointer/int/bool parameters under the x64 ABI; float
		// parameters would go in xmm registers and are not supported.
		REL::Relocation<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)> func{ REL::ID(id) };
		logger::info("re_call: ID {} ({})", id, args.dump());
		const auto result = func(values[0], values[1], values[2], values[3]);
		return { { "id", id }, { "rax", std::format("0x{:X}", result) }, { "al", result & 0xFF } };
	}
}
