#include "Render/Backend.h"

#include "Plugin/Settings.h"
#include "Render/Native/Native.h"

namespace Render::Backend
{
	namespace
	{
		// ENB (enbdev.com) runs on the game's own shaders, so with it the native backend does the
		// work. ENB sits in the game folder as d3d11.dll, a proxy for the real one, and exports its
		// SDK from there; this finds it and reads what is useful for diagnostics.
		//
		// What ENB does on its own and nothing here changes: it grades interiors and exteriors with
		// separate settings and switches when the game's current cell changes, which with seamless
		// doors is the moment the player crosses the doorway (a visible step in colour and
		// brightness, as large as the preset makes the two apart). Its screen-space effects read the
		// depth buffer, which the doorway mask writes correctly (cut pixels leave no depth).
		namespace Enb
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

			bool Present()
			{
				return sdk.module != nullptr;
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

		// The fork's exports (SEAMLESS_FORK_EXPORTS); absent ones stay null.
		struct ForkApi
		{
#define SEAMLESS_MEMBER(a_member, a_name, a_return, a_arguments) a_return(*a_member) a_arguments = nullptr;
			SEAMLESS_FORK_EXPORTS(SEAMLESS_MEMBER)
#undef SEAMLESS_MEMBER
		};

		// Found by name in the module. A fork that is ours lacking one gets it named in the log.
		void FindExports(HMODULE a_module, ForkApi& a_api, bool a_logMissing)
		{
#define SEAMLESS_FIND(a_member, a_name, a_return, a_arguments)                                                               \
	a_api.a_member = reinterpret_cast<decltype(a_api.a_member)>(::GetProcAddress(a_module, a_name));                  \
	if (a_logMissing && !a_api.a_member) {                                                                             \
		logger::warn("render: the fork has no export {}; what it does is off", a_name);                               \
	}
			SEAMLESS_FORK_EXPORTS(SEAMLESS_FIND)
#undef SEAMLESS_FIND
		}

		template <class T, std::size_t N>
		const float* Packed(float (&a_buffer)[N], const std::optional<T>& a_value)
		{
			if (!a_value) {
				return nullptr;
			}
			a_value->Pack(a_buffer);
			return a_buffer;
		}

		// Everything in the shaders of the fork of Community Shaders.
		class ForkBackend final : public Interface
		{
		public:
			explicit ForkBackend(const ForkApi& a_api) :
				api(a_api)
			{}

			const char* Name() const override { return "Community Shaders (SeamlessSkyrim fork)"; }
			bool        CanMerge() const override { return true; }
			int         MergedRooms() const override { return api.setMergedRoomDoor && api.setMergedRoomLighting ? kMaxRooms : 1; }
			bool        CanMaskGate() const override { return api.setGatePassage && api.setMergedRoomDoor; }
			bool        CanShadowPortal() const override { return api.setPortalShadows != nullptr; }
			bool        MasksPrecipitation() const override { return true; }
			// Only in its Lighting and Utility shaders.
			bool CutsPortalShader(RE::BSShader::Type a_type) const override
			{
				return api.setPortalClip && (a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility);
			}

			void SetMergedDoor(int a_room, const std::optional<DoorRect>& a_door) override
			{
				float buffer[12];
				if (api.setMergedRoomDoor) {
					api.setMergedRoomDoor(a_room, Packed(buffer, a_door));
				} else if (api.setMergedDoor) {
					api.setMergedDoor(Packed(buffer, a_door));
				}
			}

			void SetMergedLighting(int a_room, const std::optional<SpaceLight>& a_light) override
			{
				float buffer[19];
				if (api.setMergedRoomLighting) {
					api.setMergedRoomLighting(a_room, Packed(buffer, a_light));
				} else if (api.setMergedLighting) {
					api.setMergedLighting(Packed(buffer, a_light));
				}
			}

			void SetMergedFog(const std::optional<SpaceFog>& a_fog) override
			{
				float buffer[10];
				if (api.setMergedFog) {
					api.setMergedFog(Packed(buffer, a_fog));
				}
			}

			void SetFrontFog(const std::optional<SpaceFog>& a_fog) override
			{
				float buffer[10];
				if (api.setFrontFog) {
					api.setFrontFog(Packed(buffer, a_fog));
				}
			}

			void SetMergedDaylight(int a_room, const std::optional<Daylight>& a_daylight) override
			{
				float buffer[3];
				if (api.setMergedRoomDaylight) {
					api.setMergedRoomDaylight(a_room, Packed(buffer, a_daylight));
				}
			}

			void SetGatePassage(float a_depth) override
			{
				if (api.setGatePassage) {
					api.setGatePassage(a_depth);
				}
			}

			void SetInteriorSun(bool a_on) override
			{
				if (api.setInteriorSun) {
					api.setInteriorSun(a_on);
				}
			}

			void SettleSkylighting() override
			{
				if (api.settleSkylighting) {
					api.settleSkylighting();
				}
			}

			void SetPortalClip(const std::optional<ClipPlane>& a_plane) override
			{
				float buffer[4];
				if (api.setPortalClip) {
					api.setPortalClip(Packed(buffer, a_plane));
				}
			}

			void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace) override
			{
				if (api.setPortalWorldspace) {
					api.setPortalWorldspace(a_worldspace);
				}
			}

			void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps) override
			{
				if (api.setPortalShadows) {
					api.setPortalShadows(a_light, a_shadowMaps, a_volumetricShadowMaps);
				}
			}

			void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera) override
			{
				if (api.setPortalCamera) {
					const auto& frustum = a_camera->GetRuntimeData2().viewFrustum;
					api.setPortalCamera(frustum.fNear, frustum.fFar);
				}
				// The fork uploads its per-frame lighting once per frame, for the world: around the
				// portal pass it must describe the other side. The game's shaders read the sun and the
				// ambient the portal code already set, per draw.
				if (api.beginPass) {
					api.beginPass(a_sun, a_interior);
				}
			}

			void EndPortalPass() override
			{
				if (api.endPass) {
					api.endPass();
				}
			}

			bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun) override
			{
				return api.outdoorSunDirection && api.outdoorSunDirection(a_hour, a_position, a_towardSun);
			}

		private:
			ForkApi api;
		};

		// The game's own shaders, with the doorway mask and the lighting done by Render/Native.
		class NativeBackend final : public Interface
		{
		public:
			const char* Name() const override { return Enb::Present() ? "native (ENB)" : "native"; }
			bool        CanMerge() const override { return Render::Native::Ready(); }
			bool        CanMaskGate() const override { return Render::Native::Ready(); }
			// In every shader the mask hooks.
			bool CutsPortalShader(RE::BSShader::Type a_type) const override
			{
				return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility || a_type == RE::BSShader::Type::Effect;
			}

			// One room: the mask is for the room the player can walk into.
			void SetMergedDoor(int, const std::optional<DoorRect>& a_door) override { Render::Native::SetDoor(a_door); }
			void SetMergedLighting(int, const std::optional<SpaceLight>& a_light) override { Render::Native::SetMergedLighting(a_light); }
			void SetMergedFog(const std::optional<SpaceFog>& a_fog) override { Render::Native::SetMergedFog(a_fog); }
			void SetGatePassage(float a_depth) override { Render::Native::SetGatePassage(a_depth); }
			void SetPortalClip(const std::optional<ClipPlane>& a_plane) override { Render::Native::SetPortalClip(a_plane); }
			void BeginPortalPass(RE::NiDirectionalLight*, bool, const RE::NiCamera*) override { Render::Native::SetPortalPass(true); }
			void EndPortalPass() override { Render::Native::SetPortalPass(false); }

			json DescribeState() const override { return { { "mask", Render::Native::DescribeMask() }, { "lighting", Render::Native::DescribeLighting() } }; }
		};

		Interface   none;
		Interface*  current = &none;
		std::string why = "not installed";  // how the choice came about, for the log and dump_world
		bool        communityShadersLoaded = false;
		bool        communityShadersFork = false;

		std::string ReadChoice()
		{
			std::string choice;
			for (const wchar_t c : Plugin::Settings::String(Plugin::Settings::Id::kBackend)) {
				choice += static_cast<char>(std::tolower(static_cast<unsigned char>(c & 0x7F)));
			}
			return choice;
		}

		void UseNative(std::string a_why)
		{
			static NativeBackend native;
			if (Render::Native::Install()) {
				current = &native;
				why = std::move(a_why);
			} else {
				why = "the native backend could not hook the renderer";
			}
		}
	}

	void Install()
	{
		static ForkApi  forkApi;
		static std::optional<ForkBackend> fork;
		Enb::Detect();
		auto* communityShaders = ::GetModuleHandleW(L"CommunityShaders.dll");
		communityShadersLoaded = communityShaders != nullptr;
		if (communityShaders) {
			// The fork is told apart by its first export; the others are looked for then and a missing
			// one is named.
			communityShadersFork = ::GetProcAddress(communityShaders, "SeamlessPortal_SetMergedDoor") != nullptr;
			FindExports(communityShaders, forkApi, communityShadersFork);
			fork.emplace(forkApi);
		}
		const auto choice = ReadChoice();
		if (choice == "cs" || choice == "communityshaders") {
			if (communityShadersFork) {
				current = &*fork;
				why = "[Render] sBackend=cs";
			} else {
				why = communityShaders ? "[Render] sBackend=cs, but Community Shaders is not the SeamlessSkyrim fork" : "[Render] sBackend=cs, but Community Shaders isn't loaded";
			}
		} else if (choice == "native") {
			// Allowed with Community Shaders loaded, for testing: the mask doesn't depend on the
			// shaders, but Community Shaders lights the world its own way and ignores the lights
			// handed to a pass.
			UseNative(communityShaders ? "[Render] sBackend=native (Community Shaders is loaded: the room's light won't match)" : "[Render] sBackend=native");
		} else if (choice == "none" || choice == "off") {
			why = "[Render] sBackend=none";
		} else if (communityShadersFork) {
			current = &*fork;
			why = "the SeamlessSkyrim fork of Community Shaders is loaded";
		} else if (communityShaders) {
			// It replaces the game's shaders; the native backend's lighting doesn't reach them, and
			// its mask has not been tried on them. sBackend=native tries anyway.
			why = "Community Shaders is loaded but is not the SeamlessSkyrim fork";
		} else {
			UseNative(Enb::Present() ? "no Community Shaders; ENB runs on the game's own shaders" : "no Community Shaders: the game's own shaders");
		}
		logger::info("render: backend {} ({})", Name(), why);
	}

	const char* Name() { return current->Name(); }
	bool        CanMerge() { return current->CanMerge(); }
	int         MergedRooms() { return current->MergedRooms(); }
	bool        CanMaskGate() { return current->CanMaskGate(); }
	bool        CanShadowPortal() { return current->CanShadowPortal(); }
	bool        MasksPrecipitation() { return current->MasksPrecipitation(); }
	bool        CutsPortalShader(RE::BSShader::Type a_type) { return current->CutsPortalShader(a_type); }

	// Rooms past what the backend merges are not sent.
	void SetMergedDoor(int a_room, const std::optional<DoorRect>& a_door)
	{
		if (a_room >= 0 && a_room < MergedRooms()) {
			current->SetMergedDoor(a_room, a_door);
		}
	}

	void SetMergedLighting(int a_room, const std::optional<SpaceLight>& a_light)
	{
		if (a_room >= 0 && a_room < MergedRooms()) {
			current->SetMergedLighting(a_room, a_light);
		}
	}

	void SetMergedDaylight(int a_room, const std::optional<Daylight>& a_daylight)
	{
		if (a_room >= 0 && a_room < MergedRooms()) {
			current->SetMergedDaylight(a_room, a_daylight);
		}
	}

	void SetMergedFog(const std::optional<SpaceFog>& a_fog) { current->SetMergedFog(a_fog); }
	void SetFrontFog(const std::optional<SpaceFog>& a_fog) { current->SetFrontFog(a_fog); }
	void SetGatePassage(float a_depth) { current->SetGatePassage(a_depth); }
	void SetInteriorSun(bool a_on) { current->SetInteriorSun(a_on); }
	void SettleSkylighting() { current->SettleSkylighting(); }
	void SetPortalClip(const std::optional<ClipPlane>& a_plane) { current->SetPortalClip(a_plane); }
	void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace) { current->SetPortalWorldspace(a_worldspace); }

	void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps)
	{
		current->SetPortalShadows(a_light, a_shadowMaps, a_volumetricShadowMaps);
	}

	void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera) { current->BeginPortalPass(a_sun, a_interior, a_camera); }
	void EndPortalPass() { current->EndPortalPass(); }

	bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun) { return current->OutdoorSunDirection(a_hour, a_position, a_towardSun); }

	json Describe()
	{
		json out = {
			{ "backend", Name() },
			{ "why", why },
			{ "canMerge", CanMerge() },
			{ "communityShadersLoaded", communityShadersLoaded },
			{ "communityShadersFork", communityShadersFork },
			{ "mergedRooms", MergedRooms() },
			{ "masksGates", CanMaskGate() },
			{ "enb", Enb::Describe() },
		};
		out.update(current->DescribeState());
		return out;
	}
}
