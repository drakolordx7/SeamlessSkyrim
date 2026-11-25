#include "Render/Backend.h"

#include "Engine/Paths.h"
#include "Render/Enb.h"
#include "Render/Native/Native.h"

namespace
{
	using Render::Backend::Kind;

	Kind        kind = Kind::kNone;
	std::string why = "not installed";  // how the choice came about, for the log and dump_world
	bool        communityShadersLoaded = false;

	// The fork's exports (docs/COMMUNITY-SHADERS-PATCH.md); absent ones stay null.
	struct ForkApi
	{
		void (*setMergedDoor)(const float*) = nullptr;
		void (*setMergedLighting)(const float*) = nullptr;
		void (*setMergedRoomDoor)(int, const float*) = nullptr;
		void (*setMergedRoomLighting)(int, const float*) = nullptr;
		void (*setMergedFog)(const float*) = nullptr;
		void (*setFrontFog)(const float*) = nullptr;
		void (*setMergedRoomDaylight)(int, const float*) = nullptr;
		void (*setGatePassage)(float) = nullptr;
		void (*setInteriorSun)(bool) = nullptr;
		void (*settleSkylighting)() = nullptr;
		void (*setPortalClip)(const float*) = nullptr;
		void (*setPortalShadows)(RE::BSShadowDirectionalLight*, void*, void*) = nullptr;
		void (*setPortalWorldspace)(RE::TESWorldSpace*) = nullptr;
		void (*setPortalCamera)(float, float) = nullptr;
		void (*beginPass)(RE::NiDirectionalLight*, bool) = nullptr;
		void (*endPass)() = nullptr;
		bool (*outdoorSunDirection)(float, const float*, float*) = nullptr;
	} fork;

	template <class T>
	void Find(HMODULE a_module, const char* a_name, T& a_out)
	{
		a_out = reinterpret_cast<T>(::GetProcAddress(a_module, a_name));
	}

	void FindFork(HMODULE a_module)
	{
		Find(a_module, "SeamlessPortal_SetMergedDoor", fork.setMergedDoor);
		Find(a_module, "SeamlessPortal_SetMergedLighting", fork.setMergedLighting);
		Find(a_module, "SeamlessPortal_SetMergedRoomDoor", fork.setMergedRoomDoor);
		Find(a_module, "SeamlessPortal_SetMergedRoomLighting", fork.setMergedRoomLighting);
		Find(a_module, "SeamlessPortal_SetMergedFog", fork.setMergedFog);
		Find(a_module, "SeamlessPortal_SetFrontFog", fork.setFrontFog);
		Find(a_module, "SeamlessPortal_SetMergedRoomDaylight", fork.setMergedRoomDaylight);
		Find(a_module, "SeamlessPortal_SetGatePassage", fork.setGatePassage);
		Find(a_module, "SeamlessPortal_SetInteriorSun", fork.setInteriorSun);
		Find(a_module, "SeamlessPortal_SettleSkylighting", fork.settleSkylighting);
		Find(a_module, "SeamlessPortal_SetPortalClip", fork.setPortalClip);
		Find(a_module, "SeamlessPortal_SetPortalShadows", fork.setPortalShadows);
		Find(a_module, "SeamlessPortal_SetPortalWorldspace", fork.setPortalWorldspace);
		Find(a_module, "SeamlessPortal_SetPortalCamera", fork.setPortalCamera);
		Find(a_module, "SeamlessPortal_BeginPass", fork.beginPass);
		Find(a_module, "SeamlessPortal_EndPass", fork.endPass);
		Find(a_module, "SeamlessPortal_OutdoorSunDirection", fork.outdoorSunDirection);
	}

	std::string ReadChoice()
	{
		const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
		wchar_t    buffer[32]{};
		::GetPrivateProfileStringW(L"Render", L"sBackend", L"auto", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
		std::string choice;
		for (const wchar_t* c = buffer; *c; ++c) {
			choice += static_cast<char>(std::tolower(static_cast<unsigned char>(*c & 0x7F)));
		}
		return choice;
	}

	void UseNative(std::string a_why)
	{
		if (Render::Native::Install()) {
			kind = Kind::kNative;
			why = std::move(a_why);
		} else {
			kind = Kind::kNone;
			why = "the native backend could not hook the renderer";
		}
	}
}

namespace Render::Backend
{
	void Install()
	{
		Render::Enb::Detect();
		auto* communityShaders = ::GetModuleHandleW(L"CommunityShaders.dll");
		communityShadersLoaded = communityShaders != nullptr;
		if (communityShaders) {
			FindFork(communityShaders);
		}
		const bool isFork = fork.setMergedDoor != nullptr;
		const auto choice = ReadChoice();
		if (choice == "cs" || choice == "communityshaders") {
			if (isFork) {
				kind = Kind::kCommunityShaders;
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
		} else if (isFork) {
			kind = Kind::kCommunityShaders;
			why = "the SeamlessSkyrim fork of Community Shaders is loaded";
		} else if (communityShaders) {
			// It replaces the game's shaders; the native backend's lighting doesn't reach them, and
			// its mask has not been tried on them. sBackend=native tries anyway.
			why = "Community Shaders is loaded but is not the SeamlessSkyrim fork";
		} else {
			UseNative(Render::Enb::Present() ? "no Community Shaders; ENB runs on the game's own shaders" : "no Community Shaders: the game's own shaders");
		}
		logger::info("render: backend {} ({})", Name(), why);
	}

	Kind Current()
	{
		return kind;
	}

	const char* Name()
	{
		switch (kind) {
		case Kind::kCommunityShaders:
			return "Community Shaders (SeamlessSkyrim fork)";
		case Kind::kNative:
			return Render::Enb::Present() ? "native (ENB)" : "native";
		default:
			return "none";
		}
	}

	bool CanMerge()
	{
		return kind == Kind::kCommunityShaders || (kind == Kind::kNative && Render::Native::Ready());
	}

	int MergedRooms()
	{
		return kind == Kind::kCommunityShaders && fork.setMergedRoomDoor && fork.setMergedRoomLighting ? kMaxMergedRooms : 1;
	}

	void SetMergedDoor(int a_room, const float* a_door)
	{
		if (a_room < 0 || a_room >= MergedRooms()) {
			return;
		}
		if (kind == Kind::kCommunityShaders) {
			if (fork.setMergedRoomDoor) {
				fork.setMergedRoomDoor(a_room, a_door);
			} else {
				fork.setMergedDoor(a_door);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetDoor(a_door);
		}
	}

	void SetMergedLighting(int a_room, const float* a_lighting)
	{
		if (a_room < 0 || a_room >= MergedRooms()) {
			return;
		}
		if (kind == Kind::kCommunityShaders) {
			if (fork.setMergedRoomLighting) {
				fork.setMergedRoomLighting(a_room, a_lighting);
			} else if (fork.setMergedLighting) {
				fork.setMergedLighting(a_lighting);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetMergedLighting(a_lighting);
		}
	}

	void SetMergedFog(const float* a_fog)
	{
		if (kind == Kind::kCommunityShaders) {
			if (fork.setMergedFog) {
				fork.setMergedFog(a_fog);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetMergedFog(a_fog);
		}
	}

	// Not done by the native backend: both need the pixel shader.
	void SetFrontFog(const float* a_fog)
	{
		if (kind == Kind::kCommunityShaders && fork.setFrontFog) {
			fork.setFrontFog(a_fog);
		}
	}

	void SetMergedDaylight(int a_room, const float* a_daylight)
	{
		if (kind == Kind::kCommunityShaders && fork.setMergedRoomDaylight && a_room >= 0 && a_room < MergedRooms()) {
			fork.setMergedRoomDaylight(a_room, a_daylight);
		}
	}

	bool CanMaskGate()
	{
		return (kind == Kind::kCommunityShaders && fork.setGatePassage && fork.setMergedRoomDoor) || (kind == Kind::kNative && Render::Native::Ready());
	}

	void SetGatePassage(float a_depth)
	{
		if (kind == Kind::kCommunityShaders) {
			fork.setGatePassage(a_depth);
		} else if (kind == Kind::kNative) {
			Render::Native::SetGatePassage(a_depth);
		}
	}

	void SetInteriorSun(bool a_on)
	{
		if (kind == Kind::kCommunityShaders && fork.setInteriorSun) {
			fork.setInteriorSun(a_on);
		}
	}

	void SettleSkylighting()
	{
		// The game's own lighting has nothing that builds up over time like CS's skylighting.
		if (kind == Kind::kCommunityShaders && fork.settleSkylighting) {
			fork.settleSkylighting();
		}
	}

	void SetPortalClip(const float* a_plane)
	{
		if (kind == Kind::kCommunityShaders) {
			if (fork.setPortalClip) {
				fork.setPortalClip(a_plane);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalClip(a_plane);
		}
	}

	bool CanShadowPortal()
	{
		return kind == Kind::kCommunityShaders && fork.setPortalShadows;
	}

	void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps)
	{
		if (CanShadowPortal()) {
			fork.setPortalShadows(a_light, a_shadowMaps, a_volumetricShadowMaps);
		}
	}

	void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace)
	{
		if (kind == Kind::kCommunityShaders && fork.setPortalWorldspace) {
			fork.setPortalWorldspace(a_worldspace);
		}
	}

	void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera)
	{
		if (kind == Kind::kCommunityShaders) {
			if (fork.setPortalCamera) {
				const auto& frustum = a_camera->GetRuntimeData2().viewFrustum;
				fork.setPortalCamera(frustum.fNear, frustum.fFar);
			}
			// The fork uploads its per-frame lighting once per frame, for the world: around the
			// portal pass it must describe the other side. The game's shaders read the sun and the
			// ambient the portal code already set, per draw.
			if (fork.beginPass) {
				fork.beginPass(a_sun, a_interior);
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalPass(true);
		}
	}

	void EndPortalPass()
	{
		if (kind == Kind::kCommunityShaders) {
			if (fork.endPass) {
				fork.endPass();
			}
		} else if (kind == Kind::kNative) {
			Render::Native::SetPortalPass(false);
		}
	}

	bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun)
	{
		return kind == Kind::kCommunityShaders && fork.outdoorSunDirection && fork.outdoorSunDirection(a_hour, a_position, a_towardSun);
	}

	bool CutsPortalShader(RE::BSShader::Type a_type)
	{
		switch (kind) {
		case Kind::kCommunityShaders:
			return fork.setPortalClip && (a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility);
		case Kind::kNative:
			return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility || a_type == RE::BSShader::Type::Effect;
		default:
			return false;
		}
	}

	bool MasksPrecipitation()
	{
		return kind == Kind::kCommunityShaders;
	}

	json Describe()
	{
		json out = {
			{ "backend", Name() },
			{ "why", why },
			{ "canMerge", CanMerge() },
			{ "communityShadersLoaded", communityShadersLoaded },
			{ "communityShadersFork", fork.setMergedDoor != nullptr },
			{ "mergedRooms", MergedRooms() },
			{ "masksGates", CanMaskGate() },
			{ "enb", Render::Enb::Describe() },
		};
		if (kind == Kind::kNative) {
			out["mask"] = Render::Native::DescribeMask();
			out["lighting"] = Render::Native::DescribeLighting();
		}
		return out;
	}
}
