#include "Skyrim/Render/Enb.h"

// ENB (enbdev.com) runs on the game's own shaders, so the native renderer works with it
// unchanged. ENB sits in the game folder as d3d11.dll, a proxy for the real one, and exports its
// SDK from there; this finds it and reads what is useful for diagnostics.
//
// What ENB does on its own and nothing here changes: it grades interiors and exteriors with
// separate settings and switches when the game's current cell changes, which with seamless
// doors is the moment the player crosses the doorway (a visible step in colour and
// brightness, as large as the preset makes the two apart). Its screen-space effects read the
// depth buffer, which the doorway mask writes correctly (cut pixels leave no depth).
namespace Render::Enb
{
	namespace
	{
		// From the ENBSeries SDK (enbseries.h, SDK 1000+).
		enum class ParameterType : std::uint32_t
		{
			kBool = 4,
		};

		struct Parameter
		{
			std::uint8_t  data[16];
			std::uint32_t size;
			ParameterType type;
		};

		using GetLong_t = long(__stdcall*)();
		using GetParameter_t = BOOL(__stdcall*)(char* a_file, char* a_category, char* a_key, Parameter* a_out);

		struct Sdk
		{
			HMODULE        module = nullptr;
			std::string    moduleName;
			long           sdkVersion = 0;
			long           version = 0;
			long           game = 0;
			GetParameter_t getParameter = nullptr;
		} sdk;

		std::optional<bool> UseEffect()
		{
			if (!sdk.getParameter) {
				return std::nullopt;
			}
			// The SDK takes non-const strings.
			std::string file("enbseries.ini"), category("GLOBAL"), key("UseEffect");
			Parameter   value{};
			if (!sdk.getParameter(file.data(), category.data(), key.data(), &value) || value.type != ParameterType::kBool) {
				return std::nullopt;
			}
			return value.data[0] != 0;
		}
	}

	void Detect()
	{
		// ENB is usually d3d11.dll; some setups load it as dxgi.dll or through a chain loader.
		for (const auto* name : { L"d3d11.dll", L"dxgi.dll", L"d3dcompiler_46e.dll" }) {
			auto* module = ::GetModuleHandleW(name);
			if (!module) {
				continue;
			}
			const auto sdkVersion = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetSDKVersion"));
			if (!sdkVersion) {
				continue;
			}
			sdk.module = module;
			sdk.moduleName = std::filesystem::path(name).string();
			sdk.sdkVersion = sdkVersion();
			if (const auto version = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetVersion"))) {
				sdk.version = version();
			}
			if (const auto game = reinterpret_cast<GetLong_t>(::GetProcAddress(module, "ENBGetGameIdentifier"))) {
				sdk.game = game();
			}
			sdk.getParameter = reinterpret_cast<GetParameter_t>(::GetProcAddress(module, "ENBGetParameter"));
			break;
		}
		if (sdk.module) {
			const auto effects = UseEffect();
			logger::info("render: ENB {} (SDK {}) found in {}, effects {}", sdk.version, sdk.sdkVersion, sdk.moduleName, effects ? (*effects ? "on" : "off") : "unknown");
		}
	}

	json Describe()
	{
		if (!sdk.module) {
			return { { "present", false } };
		}
		const auto effects = UseEffect();
		return {
			{ "present", true },
			{ "module", sdk.moduleName },
			{ "version", sdk.version },
			{ "sdkVersion", sdk.sdkVersion },
			{ "gameIdentifier", std::format("{:X}", sdk.game) },
			{ "effects", effects ? json(*effects) : json(nullptr) },
		};
	}
}
