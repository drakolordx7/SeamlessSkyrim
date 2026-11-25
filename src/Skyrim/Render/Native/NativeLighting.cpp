#include "Skyrim/Render/Native/Native.h"

#include "Skyrim/Engine/Fog.h"
#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Render/Native/Pass.h"
#include "Skyrim/Render/SceneQuery.h"

// The light of another space with the game's own lighting shader.
//
// BSLightingShader::SetupGeometry (ID 107300) fills each draw's constants from:
// - the pass's lights: sceneLights[0] is the directional light ("sun"), sceneLights[1..] the point
//   lights, the first numShadowLights of them shadow-casting;
// - how many of those to upload, from the shader's current raw technique (bits 3-5 point lights,
//   bits 6-8 shadow lights; read at +0x94 and handed to ID 107313). The vertex and pixel shaders
//   don't depend on these bits (ID 108699 / 108700 mask them out), so changing them only changes
//   how many lights are uploaded;
// - the directional ambient, from the engine's global transform at ID 390984.
// For a pass that should be lit by another space, all three are swapped for the length of the
// engine's call and put back right after.
//
// Two kinds of pass get this:
// - the portal pass (its accumulator draws with the portal light scene, State slot 3): the engine
//   pairs lights with shapes against the main camera only, and most of the portal's shapes came
//   with no point light at all (docs/ENGINE.md, "Light scenes"). They get the nearest lights of the portal
//   scene that reach them, as the fork's strict-light path does.
// - shapes of a room merged into the world: lit by the room's lights (they stay in the portal
//   scene, so they don't light the street), and by the room's own sun and ambient when the
//   manager sends them (SetMergedLighting), scaled down as seen from outdoors.
namespace
{
	using Render::Native::LightingSwap;
	namespace Pass = Render::Native::Pass;

	constexpr int kMaxPointLights = 7;  // three technique bits; the shader's arrays hold 7

	std::mutex                         lightingLock;
	std::optional<Render::SpaceLight> mergedLighting;  // under lightingLock
	std::optional<Render::SpaceFog>   mergedFog;       // under lightingLock

	struct Stats
	{
		std::uint64_t portalPasses = 0;
		std::uint64_t mergedPasses = 0;
		std::uint64_t lightsGiven = 0;
		std::uint64_t ownLight = 0;  // merged passes lit with the room's own sun and ambient
		std::uint64_t ownFog = 0;    // merged passes fogged with the street's fog
	} stats;

	// The fog the lighting shader's technique setup (ID 107320, from SetupTechnique) uploads is the
	// active light scene's fog property (Engine/Fog.h).
	constexpr std::size_t kFogBegin = Engine::Fog::kBegin;
	constexpr std::size_t kFogEnd = Engine::Fog::kEnd;

	// The directional ambient transform the lighting shader's setup reads (rotation, translation, scale).
	RE::NiTransform& AmbientTransform()
	{
		static REL::Relocation<RE::NiTransform*> transform{ REL::RelocationID(513243, 390984) };
		return *transform.get();
	}

	// The SSAO composite (BSImagespaceShader::RenderSSAO, ID 107245) fogs the whole picture by
	// depth with the active scene's fog: with the street merged into an interior, the interior's
	// (near, black) turned the distant mountains seen through the doorway black. While the street's
	// fog is set, the composite gets a copy of the fog property carrying it; the interior itself
	// lies well inside the street's fog distance and is barely touched. Only then: the composite
	// fogs every pixel alike, and a room merged into the street (its own fog set for its shapes)
	// would have fogged the street as the room.
	struct CompositeFogHook
	{
		static void* Thunk(int a_index)
		{
			auto* property = static_cast<std::byte*>(func(a_index));
			std::optional<Render::SpaceFog> fog;
			{
				std::scoped_lock lock(lightingLock);
				fog = mergedFog;
			}
			auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			const bool streetMerged = here && here->IsInteriorCell();
			if (!property || !fog || !streetMerged || !Render::Native::MergedDoorSet()) {
				return property;
			}
			alignas(16) static std::byte copy[0x90];
			std::memcpy(copy, property, sizeof(copy));
			Engine::Fog::Write(copy, *fog);
			return copy;
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	struct ScaledLight
	{
		RE::NiLight* light;
		RE::NiColor  diffuse;
	};
}

struct Render::Native::LightingSwap::Saved
{
	RE::BSRenderPass*     pass = nullptr;
	RE::BSLightingShader* shader = nullptr;
	RE::BSLight**         sceneLights = nullptr;
	std::uint8_t          numLights = 0;
	std::uint8_t          numShadowLights = 0;
	std::uint32_t         technique = 0;
	RE::BSLight*          lights[1 + kMaxPointLights]{};
	ScaledLight           scaled[kMaxPointLights]{};
	int                   scaledCount = 0;

	std::byte*                             fog = nullptr;  // the fog property changed for this pass
	std::array<std::byte, kFogEnd - kFogBegin> fogBytes{};

	RE::NiDirectionalLight* sun = nullptr;
	RE::NiColor             sunDiffuse;
	float                   sunFade = 0.0f;
	RE::NiPoint3            sunDirection;
	RE::NiMatrix3           sunRotate;
	bool                    ambientSwapped = false;
	RE::NiTransform         ambient;
};

namespace
{
	// SetupGeometry calls never nest; one set of saved state does.
	Render::Native::LightingSwap::Saved storage;
	bool                                storageInUse = false;

	struct Candidate
	{
		float        distance;
		RE::BSLight* light;
	};

	// The nearest lights of a_scene whose radius reaches the shape.
	int PickLights(RE::ShadowSceneNode* a_scene, const RE::BSGeometry* a_shape, RE::BSLight** a_out)
	{
		static std::vector<Candidate> candidates;
		candidates.clear();
		const auto& bound = a_shape->worldBound;
		const auto  consider = [&](RE::BSLight* a_light) {
			auto* ni = a_light ? a_light->light.get() : nullptr;
			if (!ni) {
				return;
			}
			const float distance = ni->world.translate.GetDistance(bound.center) - bound.radius;
			if (distance < ni->GetLightRuntimeData().radius.x) {
				candidates.push_back({ distance, a_light });
			}
		};
		auto& data = a_scene->GetRuntimeData();
		for (auto& light : data.activeLights) {
			consider(light.get());
		}
		for (auto& light : data.activeShadowLights) {
			consider(light.get());
		}
		const auto count = std::min<std::size_t>(candidates.size(), kMaxPointLights);
		std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
		for (std::size_t i = 0; i < count; ++i) {
			a_out[i] = candidates[i].light;
		}
		return static_cast<int>(count);
	}
}

namespace Render::Native
{
	LightingSwap::LightingSwap(RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
	{
		if (storageInUse || !a_pass || !a_pass->geometry || a_pass->numLights < 1 || !a_pass->sceneLights || !a_pass->sceneLights[0]) {
			return;
		}
		auto*      portalScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[Render::kPortalSceneSlot];
		const bool portal = Pass::InPortalScene();
		const bool merged = !portal && Pass::IsMergedRoomShape(a_pass->geometry);
		if (!portal && !merged) {
			return;
		}
		std::optional<Render::SpaceLight> lighting;
		std::optional<Render::SpaceFog>   fog;
		if (merged) {
			std::scoped_lock lock(lightingLock);
			lighting = mergedLighting;
			fog = mergedFog;
		}
		// The room's lights are in the portal scene; a street merged into an interior keeps its
		// own in the world's (RemoteCell doesn't adopt them), and is left with the engine's pairing.
		const auto& remote = Render::Scene().RemoteCells();
		const bool  giveLights = portal || (!remote.empty() && remote.front()->IsInteriorCell());
		const float scale = lighting ? lighting->scale : 1.0f;

		storageInUse = true;
		saved = &storage;
		auto& s = *saved;
		s = {};
		s.pass = a_pass;
		s.shader = static_cast<RE::BSLightingShader*>(a_shader);

		// The street merged into an interior: the interior's fog (near, black) turned the distant
		// mountains black. Fog is uploaded with the technique, not per shape, so the technique is
		// set up again with the street's fog in the scene's fog property; the destructor puts the
		// property back and makes the next pass set its technique up again.
		if (fog) {
			if (auto* property = Engine::Fog::ActiveProperty()) {
				s.fog = property;
				std::memcpy(s.fogBytes.data(), property + kFogBegin, s.fogBytes.size());
				Engine::Fog::Write(property, *fog);
				a_shader->SetupTechnique(a_pass->passEnum);
				++stats.ownFog;
			}
		}
		s.sceneLights = a_pass->sceneLights;
		s.numLights = a_pass->numLights;
		s.numShadowLights = a_pass->numShadowLights;
		s.technique = s.shader->currentRawTechnique;

		if (giveLights && portalScene) {
			s.lights[0] = a_pass->sceneLights[0];
			const int count = PickLights(portalScene, a_pass->geometry, s.lights + 1);
			// No shadow maps for these: a shadow-casting light would shine through walls, so it is
			// dimmed by how much of it reaches the shape (LightOcclusion), as the fork does.
			for (int i = 0; i < count; ++i) {
				auto* ni = s.lights[1 + i]->light.get();
				float factor = scale;
				if (s.lights[1 + i]->IsShadowLight()) {
					factor *= Portal::LightOcclusion::Visibility(a_pass->geometry, ni);
				}
				if (factor != 1.0f) {
					auto& diffuse = ni->GetLightRuntimeData().diffuse;
					s.scaled[s.scaledCount++] = { ni, diffuse };
					diffuse = diffuse * factor;
				}
			}
			a_pass->sceneLights = s.lights;
			a_pass->numLights = static_cast<std::uint8_t>(1 + count);
			a_pass->numShadowLights = 0;
			s.shader->currentRawTechnique = (s.technique & ~0x1F8u) | (static_cast<std::uint32_t>(count) << 3);
			stats.lightsGiven += count;
		}
		if (lighting) {
			auto* sunLight = s.sceneLights[0]->light.get();
			s.sun = sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sunLight) : nullptr;
			if (s.sun) {
				auto& data = s.sun->GetLightRuntimeData();
				auto& direction = s.sun->GetDirectionalLightRuntimeData().worldDir;
				s.sunDiffuse = data.diffuse;
				s.sunFade = data.fade;
				s.sunDirection = direction;
				s.sunRotate = s.sun->world.rotate;
				data.diffuse = { lighting->sun.red * scale, lighting->sun.green * scale, lighting->sun.blue * scale };
				data.fade = 1.0f;
				// The light shines along its direction; the array has the way towards it.
				const RE::NiPoint3 along{ -lighting->towardSun.x, -lighting->towardSun.y, -lighting->towardSun.z };
				direction = along;
				for (int row = 0; row < 3; ++row) {
					s.sun->world.rotate.entry[row][0] = along[row];
				}
			}
			auto& ambient = AmbientTransform();
			s.ambient = ambient;
			s.ambientSwapped = true;
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 3; ++column) {
					ambient.rotate.entry[row][column] = lighting->ambientRotate.entry[row][column] * scale;
				}
				ambient.translate[row] = lighting->ambientTranslate[row] * scale;
			}
			++stats.ownLight;
		}
		++(portal ? stats.portalPasses : stats.mergedPasses);
	}

	LightingSwap::~LightingSwap()
	{
		if (!saved) {
			return;
		}
		auto& s = *saved;
		s.pass->sceneLights = s.sceneLights;
		s.pass->numLights = s.numLights;
		s.pass->numShadowLights = s.numShadowLights;
		s.shader->currentRawTechnique = s.technique;
		for (int i = 0; i < s.scaledCount; ++i) {
			s.scaled[i].light->GetLightRuntimeData().diffuse = s.scaled[i].diffuse;
		}
		if (s.sun) {
			auto& data = s.sun->GetLightRuntimeData();
			data.diffuse = s.sunDiffuse;
			data.fade = s.sunFade;
			s.sun->GetDirectionalLightRuntimeData().worldDir = s.sunDirection;
			s.sun->world.rotate = s.sunRotate;
		}
		if (s.ambientSwapped) {
			AmbientTransform() = s.ambient;
		}
		if (s.fog) {
			std::memcpy(s.fog + kFogBegin, s.fogBytes.data(), s.fogBytes.size());
			// SetupAndDrawPass (ID 107644) sets a technique up only when it differs from the last
			// one (ID 415272); forgotten, the next shape sets it up again, with the world's fog.
			static REL::Relocation<std::uint32_t*> lastTechnique{ REL::RelocationID(528320, 415272) };
			*lastTechnique.get() = 0;
		}
		storageInUse = false;
	}

	void SetMergedLighting(const std::optional<SpaceLight>& a_light)
	{
		std::scoped_lock lock(lightingLock);
		mergedLighting = a_light;
	}

	void InstallLightingHooks()
	{
		REL::Relocation<std::uintptr_t> site{ REL::RelocationID(100520, 107245), REL::Relocate(0xC37, 0xD26) };
		if (Hooks::CallsTo(site.address(), REL::RelocationID(98984, 105637), "native backend: SSAO composite's fog")) {
			CompositeFogHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), CompositeFogHook::Thunk);
		}
	}

	void SetMergedFog(const std::optional<SpaceFog>& a_fog)
	{
		std::scoped_lock lock(lightingLock);
		mergedFog = a_fog;
	}

	json DescribeLighting()
	{
		bool own = false;
		bool fog = false;
		{
			std::scoped_lock lock(lightingLock);
			own = mergedLighting.has_value();
			fog = mergedFog.has_value();
		}
		return {
			{ "mergedOwnLighting", own },
			{ "portalPasses", stats.portalPasses },
			{ "mergedPasses", stats.mergedPasses },
			{ "lightsGiven", stats.lightsGiven },
			{ "ownLightPasses", stats.ownLight },
			{ "mergedFog", fog },
			{ "ownFogPasses", stats.ownFog },
		};
	}
}
