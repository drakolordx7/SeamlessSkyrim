#include "Portal/PortalRender.h"
#include "Portal/Travel.h"

#include "Diag/Telemetry.h"
#include "Hooks.h"

#include "Portal/CameraLimit.h"
#include "Portal/DoorPair.h"
#include "Portal/DoorwaySnapshot.h"
#include "Portal/LightOcclusion.h"
#include "Portal/LightScene.h"
#include "Portal/MergedCull.h"
#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Portal/RemoteCell.h"
#include "Render/Backend.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	using Portal::Manager::Active;

	struct FileRequest
	{
		std::string                         path;
		std::shared_ptr<std::promise<json>> done;
	};

	std::mutex                 stateLock;
	std::optional<FileRequest> pendingFile;
	json                       lastFrame;

	// 0x400 = RENDER_FLAG::kUpdateViewport.
	// 0x4: without 0x4 or 0x80 one of the shaders' SetupGeometry (ID 107528) switches the depth mode
	// to "test EQUAL, no write", which only works after a depth prepass. The portal pass has none,
	// the mode then stuck for the rest of the pass and nothing was drawn whenever such an object was
	// in view (most views into the Sleeping Giant Inn: the doorway was black).
	// 0x200: the menu-3D renderer (ID 52727) passes it too. Without 0x200 or 0x80 the accumulator's
	// post-accumulation stage (ID 106584) runs a world-only step under some conditions and then
	// binds the world's main colour target for the rest of the pass, so the portal's blended and
	// effect geometry was drawn straight into the main picture.
	// 0x400: the viewport is the whole target. A picture for the doorway on screen leaves it out and
	// adds 0x1 (the main view's jitter) instead: see LiveRenderFlags.
	constexpr std::uint32_t kRenderFlags = 0x400 | 0x200 | 0x4;

	// The picture shown in the doorway is drawn like the main view: at its render resolution (an
	// upscaler or dynamic resolution draws the scene into the top-left part of its target) and with
	// its subpixel jitter, so the upscaler resolves the doorway with the scene around it. Drawn at
	// the full target size without jitter and point-sampled into the smaller scene, it showed
	// staircased edges and shimmering foliage the upscaler couldn't smooth.
	constexpr std::uint32_t LiveRenderFlags()
	{
		return (kRenderFlags & ~0x400u) | 0x1;
	}

	// The fraction of its target the main view is drawn into (1 without an upscaler or dynamic
	// resolution).
	std::array<float, 2> RenderScale()
	{
		const auto& state = RE::BSGraphics::State::GetSingleton()->GetRuntimeData();
		return { state.dynamicResolutionWidthRatio > 0.0f ? state.dynamicResolutionWidthRatio : 1.0f,
			state.dynamicResolutionHeightRatio > 0.0f ? state.dynamicResolutionHeightRatio : 1.0f };
	}

	RE::NiPointer<RE::NiCamera> portalCamera;
	// Ref-counted owner: the culling context also holds a reference, and when it drops it the
	// accumulator would otherwise be freed.
	RE::NiPointer<RE::BSShaderAccumulator> portalAccumulator;
	float                                  defaultNear = 0.0f;

	// The engine's targets whose table entries the portal pass takes over, with textures of its own
	// (see OwnTargets; the colour made like the main scene's).
	constexpr auto kColorTarget = RE::RENDER_TARGETS::kMENUBG;
	constexpr auto kDepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY;

	// The engine passes the RenderTargetManager object itself (&ID 411451) as `this` to these
	// (AE 1.7.104, seen in ID 52727); CommonLib's wrappers pass a different object. SetColorTarget's
	// function ignores it on both runtimes (docs/ENGINE.md).
	void* RenderTargetManagerObject()
	{
		static REL::Relocation<std::uintptr_t> object{ REL::RelocationID(524970, 411451) };
		return reinterpret_cast<void*>(object.address());
	}

	void* RendererObject()
	{
		static REL::Relocation<std::uintptr_t> object{ REL::RelocationID(524907, 411393) };
		return reinterpret_cast<void*>(object.address());
	}

	void SetColorTarget(std::uint32_t a_slot, std::uint32_t a_target, std::uint32_t a_mode, bool a_updateViewport)
	{
		using func_t = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(75646, 77453) };
		func(RenderTargetManagerObject(), a_slot, a_target, a_mode, a_updateViewport);
	}

	void SetDepthTarget(std::uint32_t a_target, std::uint32_t a_mode, std::uint32_t a_slice)
	{
		using func_t = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(75647, 77454) };
		func(RenderTargetManagerObject(), a_target, a_mode, a_slice, false);
	}

	// Empties what a cull put into the accumulator. Without this, rendering every frame piled new
	// passes onto the previous frame's stale ones and crashed in the batch renderer. Mirrors the
	// cleanup Main::RenderPlayerView / RenderWorld do for the main accumulator.
	void ResetAccumulator(RE::BSShaderAccumulator* a_accumulator)
	{
		static REL::Relocation<void (*)(RE::BSShaderAccumulator*)>                clearItems{ REL::RelocationID(99952, 106597) };  // NiBackToFrontAccumulator::ClearItemsList
		static REL::Relocation<void (*)(RE::BSShaderAccumulator*, std::int32_t)> clearGroup{ REL::RelocationID(99955, 106600) };
		clearItems(a_accumulator);
		clearGroup(a_accumulator, 6);
		clearGroup(a_accumulator, 5);
	}

	// SetupAndDrawPass call in the pass loop of BSBatchRenderer::RenderBatches (ID 107642); CS
	// already redirects this call, so ours wraps theirs. Safety net for a crash that is not fully
	// understood yet: the portal pass has drawn render passes that were already returned to the
	// pool (shader gone), which crashed in the hooks further down. Skip those and say so.
	std::atomic_bool inPortalSubmit = false;
	std::uint32_t    skippedFreedPasses = 0;
	std::uint32_t    freedPassesThisSubmit = 0;
	// Passes drawn by the current portal submit. The batch renderer walks a group's passes along
	// BSRenderPass::passGroupNext (read after each draw, ID 107642 +0x294); a pass met twice means
	// the chain runs in a circle, and the game froze with the main thread drawing it for ever.
	std::unordered_set<const RE::BSRenderPass*> drawnThisSubmit;
	std::uint32_t                               cutCycles = 0;
	std::uint32_t    portalFrame = 0;  // portal frames drawn for the current remote space
	bool             reportedPassCycle = false;
	// Diagnostics (portal_light_debug logPasses): the shapes drawn in the next portal pass.
	std::atomic_bool                   logPassesNext = false;
	std::map<std::string, std::uint32_t> loggedPasses;

	// Reads a pointer without trusting the address it is read from.
	bool ReadPointer(const void* a_address, std::uintptr_t& a_value)
	{
		__try {
			a_value = *static_cast<const std::uintptr_t*>(a_address);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Every real shader is one of the game's few singletons: once one has been seen with a vtable
	// in SkyrimSE.exe it is known, and anything else is checked without trusting it.
	bool ValidShader(const RE::BSShader* a_shader)
	{
		static const auto [begin, end] = [] {
			const auto& module = REL::Module::get();
			return std::pair{ module.base(), module.base() + module.segment(REL::Segment::rdata).offset() + module.segment(REL::Segment::rdata).size() };
		}();
		static std::vector<const RE::BSShader*> known;
		if (!a_shader) {
			return false;
		}
		if (std::ranges::find(known, a_shader) != known.end()) {
			return true;
		}
		std::uintptr_t vtable = 0;
		if (!ReadPointer(a_shader, vtable) || vtable < begin || vtable >= end) {
			return false;
		}
		if (known.size() < 64) {
			known.push_back(a_shader);
		}
		return true;
	}

	// A pass reached through a link, which may point at a pass returned to the pool: nothing of it
	// is trusted, not even that its address is mapped.
	bool PassLooksValid(const RE::BSRenderPass* a_pass)
	{
		std::uintptr_t shader = 0;
		return ReadPointer(&a_pass->shader, shader) && ValidShader(reinterpret_cast<const RE::BSShader*>(shader));
	}

	// The far door's shapes (see RenderPortal): never drawn by the portal pass, whatever brings
	// them there, in case culling its root isn't enough.
	std::unordered_set<const void*> portalHiddenShapes;
	std::uint32_t                   portalHiddenDrawn = 0;

	// The far door's plane for the portal pass being drawn (unit normal away from the portal camera,
	// offset): what lies on the camera's side of it can't be seen through the doorway.
	std::optional<std::pair<RE::NiPoint3, float>> farDoorClip;

	// The render backend cuts the portal pass at that plane per pixel, but only for some shaders
	// (the patched Community Shaders in its Lighting and Utility shaders; the native backend also
	// in effect shaders). The others (glow cards such as the city's window glows, particles, grass)
	// weren't cut: between the portal camera and the far doorway they were drawn into the picture,
	// more of them the closer the player got to the door. Shapes drawn with those that lie wholly
	// on the camera's side are left out (a cut would have removed all of them anyway).
	bool UncutShapeBeforeFarDoor(const RE::BSRenderPass* a_pass)
	{
		if (!farDoorClip || !a_pass->shader || !a_pass->geometry) {
			return false;
		}
		const auto type = a_pass->shader->shaderType.get();
		if (::Render::Backend::CutsPortalShader(type)) {
			return false;
		}
		const auto& [normal, offset] = *farDoorClip;
		const auto& bound = a_pass->geometry->worldBound;
		if (bound.center.Dot(normal) + bound.radius >= offset) {
			return false;
		}
		static std::unordered_set<const RE::BSGeometry*> reported;
		if (reported.size() < 40 && reported.insert(a_pass->geometry).second) {
			logger::info("portal: '{}' (shader type {}) left out of the portal pass: before the far door, where its shader isn't cut", a_pass->geometry->name.c_str(),
				static_cast<int>(type));
		}
		return true;
	}

	struct DrawPassHook
	{
		static void Thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
		{
			if (inPortalSubmit && logPassesNext && a_pass && a_pass->geometry) {
				loggedPasses[std::format("{} [{:X}]", a_pass->geometry->name.c_str(), a_technique)]++;
			}
			// A pass whose memory went back to the pool may still name a "shader" that is garbage
			// by now (its vtable 0: crashed walking out of the Sleeping Giant with the street
			// behind the door; or a pointer into nothing: crashed reading it).
			if (inPortalSubmit && a_pass && !PassLooksValid(a_pass)) {
				if (skippedFreedPasses++ < 200) {
					// Nothing of the pass is read: it may be memory reused for anything.
					logger::warn("portal: skipped a freed render pass ({:X}, technique {:X}) in portal frame {}", reinterpret_cast<std::uintptr_t>(a_pass), a_technique, portalFrame);
				}
				// Only a group's first pass gets here (the chains into freed passes are cut before
				// them, below), and the engine follows its link next. Freed passes can link back to
				// each other, and the loop then never ends (the game froze right after a crossing,
				// the main thread spinning here). Writing into a pass that may be someone else's by
				// now is the last resort against that hang, after more of them than any real list
				// holds.
				if (++freedPassesThisSubmit > 256) {
					a_pass->next = nullptr;
					a_pass->passGroupNext = nullptr;
					if (!std::exchange(reportedPassCycle, true)) {
						logger::error("portal: render pass list loops through freed passes; cut");
					}
				}
				return;
			}
			if (inPortalSubmit && a_pass && (portalHiddenShapes.contains(a_pass->geometry) || UncutShapeBeforeFarDoor(a_pass))) {
				portalHiddenDrawn += portalHiddenShapes.contains(a_pass->geometry) ? 1 : 0;
				CutBeforeFreed(a_pass);
				return;
			}
			if (inPortalSubmit && a_pass && !drawnThisSubmit.insert(a_pass).second) {
				a_pass->passGroupNext = nullptr;
				if (cutCycles++ < 20) {
					logger::error("portal: render pass {:X} drawn twice in one portal pass (the group's chain loops); chain cut", reinterpret_cast<std::uintptr_t>(a_pass));
				}
				return;
			}
			// Water (an exterior's river seen from inside) depends on the world pass's water state
			// (reflections, the water's own render targets), which this pass doesn't have. Left out
			// for now.
			if (inPortalSubmit && a_pass && a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Water) {
				CutBeforeFreed(a_pass);
				return;
			}
			func(a_pass, a_technique, a_alphaTest, a_renderFlags);
			if (inPortalSubmit && a_pass) {
				CutBeforeFreed(a_pass);
			}
		}

		// The engine follows passGroupNext once this returns. Pointing into a freed pass, the
		// chain is ended here, on a pass known to be live, instead of on the freed one: what
		// follows it is drawn by the second portal pass (counted as freed, see RenderWorldHook).
		static void CutBeforeFreed(RE::BSRenderPass* a_pass)
		{
			auto* next = a_pass->passGroupNext;
			if (next && !PassLooksValid(next)) {
				a_pass->passGroupNext = nullptr;
				++freedPassesThisSubmit;
				if (skippedFreedPasses++ < 200) {
					logger::warn("portal: chain cut before a freed render pass ({:X}) in portal frame {}", reinterpret_cast<std::uintptr_t>(next), portalFrame);
				}
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Effect shaders that read the scene depth (soft particles and the like) unbind the depth
	// buffer while they draw and afterwards rebind "the world's depth target" (ID 107522 + 0x6A
	// asks ID 77457 which one that is). Inside the portal pass that put the main view's depth
	// buffer in place of the portal's for everything drawn after the first such effect.
	struct WorldDepthTargetHook
	{
		static std::uint32_t Thunk(void* a_renderTargetManager)
		{
			return inPortalSubmit ? static_cast<std::uint32_t>(kDepthTarget) : func(a_renderTargetManager);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// "Are there passes in this technique range?" as asked by the accumulator's draw routine
	// (ID 106583 + 0x3B4) for the range 0x5C00006D..70: the refraction-style effects (heat haze
	// over a fire pit). If so, the routine rebinds the *world's* render targets to draw them and
	// leaves them bound. Inside the portal pass that put parts of the interior straight into the
	// main picture; the portal does without those effects instead.
	struct RefractionPassesHook
	{
		static bool Thunk(RE::BSBatchRenderer* a_batch, std::uint32_t a_first, std::uint32_t a_last)
		{
			return inPortalSubmit ? false : func(a_batch, a_first, a_last);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	struct LightDebug
	{
		float ambientScale = 1.0f;
		float sunScale = 1.0f;
		float shadowLightScale = 1.0f;
		float pointLightScale = 1.0f;
		bool  communityShaders = true;
		bool  precipitation = true;   // rain and snow in the portal pass of an outdoor space
		bool  sunShadows = true;      // the sun's shadows in the portal pass of an outdoor space
		bool  tintDoorway = false;    // the doorway's picture tinted red: tells which flashes are in it
		// Switches for finding a whole-screen flash (the in-game menu): each leaves one part out.
		bool  skipPortalPass = false;   // no portal pass, nothing in the doorway
		bool  skipComposite = false;    // the pass is drawn, the picture not put into the doorway
		bool  skipMotion = false;       // the picture put in without motion vectors for the upscaler
	} lightDebug;

	// The main view's camera as the renderer held it when Main::RenderWorld began, which the doorway's
	// picture is placed with. When the world is done the renderer may hold another camera: every
	// other frame something inside the world pass (a camera at (0,0,1) looking elsewhere) left its
	// own there. The quad placed with it was behind that camera, so it wasn't drawn at all, and the
	// doorway showed what the room has behind its door (a box with a closed door) every second
	// frame: the flashing seen standing at a door in first person.
	using ShadowStateData = std::remove_reference_t<decltype(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData())>;
	struct MainView
	{
		decltype(std::declval<ShadowStateData>().posAdjust)         posAdjust;
		decltype(std::declval<ShadowStateData>().previousPosAdjust) previousPosAdjust;
		decltype(std::declval<ShadowStateData>().cameraData)        cameraData;
	};
	MainView mainView;

	// ---- rain and snow behind a door ----------------------------------------------------------
	// The weather's precipitation is one particle shape (Sky::precip, under the world scene's
	// "Weather" node) that the engine keeps centred on the camera. Rain writes no depth, so the
	// doorway's picture, put over the main view after the world is drawn, covered the rain in front
	// of a city gate too: it rained up to the gateway's outline and not in it. Both sides of a gate
	// share the weather and the coordinates, so the main view's own rain is drawn into the picture,
	// with the main camera and against the main view's depth: in the doorway it is then exactly the
	// rain the picture covers. (The far side's rain drawn from the portal camera was only the far,
	// thin streaks: its near plane stands at the far gate.)
	void DrawMainPrecipitation(RE::ShadowSceneNode* a_root)
	{
		// Not cut at the far door: this rain is the main view's, in the main view's coordinates.
		::Render::Backend::EndPortalPass();
		::Render::Backend::SetPortalClip(nullptr);
		auto* sun = a_root->GetRuntimeData().sunLight;
		auto* camera = RE::Main::WorldRootCamera();
		::Render::Backend::BeginPortalPass(sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr, false, camera);
		auto* precip = RE::Sky::GetSingleton()->precip;
		ResetAccumulator(portalAccumulator.get());
		auto& data = portalAccumulator->GetRuntimeData();
		data.activeShadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		data.eyePosition = camera->world.translate;
		for (auto* shape : { precip->currentPrecip.get(), precip->lastPrecip.get() }) {
			if (shape && !shape->GetAppCulled()) {
				RE::BSCullingProcess::CullingContext context;
				context.accumulator.reset(portalAccumulator.get());
				context.camera.reset(camera);
				context.scene.reset(shape);
				RE::BSCullingProcess::Process(context);
			}
		}
		SetDepthTarget(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, RE::BSGraphics::SRTM_NO_CLEAR, 0);
		inPortalSubmit = true;
		// In the live picture's viewport, pixel for pixel over the main view's depth.
		RE::BSGraphics::Renderer::SubmitAccumulator(camera, portalAccumulator.get(), LiveRenderFlags());
		inPortalSubmit = false;
		SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_NO_CLEAR, 0);
	}

	// The sky's precipitation update (ID 26179, from Sky::Update) gives the rain and snow no
	// particles unless the sky is in full mode (the emitter's density 0, the shape culled), and
	// indoors it is in dome-only mode. While the street is merged in, the update runs as outdoors:
	// the street through the doorway has the weather's rain, fading in and out with the weather as
	// it does outdoors. The shape follows the camera into the room; the doorway mask keeps it to the
	// street (Manager::MergedRoomOf).
	struct PrecipitationUpdateHook
	{
		static void Thunk(RE::Precipitation* a_precip, float a_delta)
		{
			auto*      sky = RE::Sky::GetSingleton();
			const auto mode = sky->mode;
			if (Portal::Manager::StreetMerged() && ::Render::Backend::MasksPrecipitation()) {
				sky->mode = RE::Sky::Mode::kFull;
			}
			original(a_precip, a_delta);
			sky->mode = mode;
		}

		// Called from two places (Sky::Update and ID 26222), so the function itself is redirected:
		// its first instructions (mov rax, rsp; push rbx; sub rsp, imm32: 11 bytes, position
		// independent, the frame size differing between builds) are moved to a stub that continues
		// behind them.
		static void Install()
		{
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(25638, 26179) };
			constexpr std::size_t           kMoved = 11;
			const auto*                     code = reinterpret_cast<const std::uint8_t*>(target.address());
			static constexpr std::uint8_t   expected[7] = { 0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC };
			if (std::memcmp(code, expected, sizeof(expected)) != 0 || code[9] != 0 || code[10] != 0) {
				logger::warn("portal: precipitation update not as expected; no rain or snow through a doorway seen from indoors");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(kMoved + 14));
			std::memcpy(stub, code, kMoved);
			const std::uint8_t jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };  // jmp [rip+0]
			std::memcpy(stub + kMoved, jump, sizeof(jump));
			const std::uintptr_t resume = target.address() + kMoved;
			std::memcpy(stub + kMoved + sizeof(jump), &resume, sizeof(resume));
			original = reinterpret_cast<decltype(original)>(stub);
			trampoline.write_branch<5>(target.address(), Thunk);
		}
		static inline void (*original)(RE::Precipitation*, float) = nullptr;
	};

	// ---- the remote cell's lighting environment ----------------------------------------------
	// Lighting shaders read one global directional ambient, set each frame for the current space
	// by Sky::SetDirectionalAmbientColors (ID 105643, colors[3][2] + specular tint + fresnel). The
	// portal's space needs its own, so it is set for the portal pass and the world's put back.
	struct AmbientState
	{
		float colors[3][2][3];  // [x/y/z][max/min][rgb]
		float tint[3];
		float fresnel;
	};

	// A space's own colours, through the engine (and Community Shaders' hook on it, which adjusts
	// them as it does for the engine's own calls).
	void ApplyAmbient(const AmbientState& a_state)
	{
		static REL::Relocation<void (*)(const float*, const float*, float)> setAmbient{ REL::RelocationID(98989, 105643) };
		setAmbient(&a_state.colors[0][0][0], a_state.tint, a_state.fresnel);
	}

	// Everything ID 105643 writes, as memory: a copy of its inputs (ID 414646, 0x48 bytes) and what
	// it builds from them (from ID 390984: the ambient transform, then tint and fresnel, 17 floats).
	// The world's ambient is put back from this, not by calling the function again with its saved
	// inputs: those are the colours after Community Shaders' hook adjusted them (Effects11), and
	// passed in again they were adjusted twice. The outdoor ambient changed, every frame a portal
	// was drawn from inside and every second a room was merged in.
	struct AmbientSnapshot
	{
		std::array<std::byte, 0x48> inputs;
		std::array<std::byte, 0x44> built;
	};

	std::pair<std::byte*, std::byte*> AmbientMemory()
	{
		static const auto memory = [] {
			auto* inputs = reinterpret_cast<std::byte*>(REL::RelocationID(527705, 414646).address());
			auto* built = reinterpret_cast<std::byte*>(REL::RelocationID(513243, 390984).address());
			// The layout read off the function (AE 1.7.104): both blocks contiguous.
			SEAMLESS_ASSERT(REL::RelocationID(527716, 414657).address() - REL::RelocationID(527705, 414646).address() == 0x44);
			SEAMLESS_ASSERT(REL::RelocationID(513259, 391000).address() - REL::RelocationID(513243, 390984).address() == 0x40);
			return std::pair{ inputs, built };
		}();
		return memory;
	}

	// The directional ambient transform ID 105643 builds from six colours, worked out the same way
	// without calling it: a room merged into the world got its own by setting the engine's global
	// ambient to the room's for a moment, through Community Shaders' hook, and the outdoors
	// flashed white when a room was merged in. Per colour channel c and axis a:
	// rotate[c][a] = (min - max) / 2, translate[c] = (K[c] + sum over the axes of (max + min)) / 6,
	// with K the engine's globals ID 410515..410517.
	RE::NiTransform AmbientTransform(const AmbientState& a_state)
	{
		static REL::Relocation<const float*> offsets{ REL::RelocationID(523935, 410515) };
		SEAMLESS_ASSERT(REL::RelocationID(523937, 410517).address() - REL::RelocationID(523935, 410515).address() == 8);  // three floats in a row
		RE::NiTransform transform;
		for (int channel = 0; channel < 3; ++channel) {
			float sum = offsets.get()[channel];
			for (int axis = 0; axis < 3; ++axis) {
				const float max = a_state.colors[axis][0][channel];
				const float min = a_state.colors[axis][1][channel];
				transform.rotate.entry[channel][axis] = 0.5f * (min - max);
				sum += max + min;
			}
			transform.translate[channel] = sum / 6.0f;
		}
		return transform;
	}

	AmbientSnapshot SaveAmbient()
	{
		const auto [inputs, built] = AmbientMemory();
		AmbientSnapshot snapshot;
		std::memcpy(snapshot.inputs.data(), inputs, snapshot.inputs.size());
		std::memcpy(snapshot.built.data(), built, snapshot.built.size());
		return snapshot;
	}

	void RestoreAmbient(const AmbientSnapshot& a_snapshot)
	{
		const auto [inputs, built] = AmbientMemory();
		std::memcpy(inputs, a_snapshot.inputs.data(), a_snapshot.inputs.size());
		std::memcpy(built, a_snapshot.built.data(), a_snapshot.built.size());
	}

	// The cell's directional ambient (DALC), from its lighting template when it inherits ambient.
	std::optional<AmbientState> CellAmbient(RE::TESObjectCELL* a_cell)
	{
		auto* own = a_cell ? a_cell->GetLighting() : nullptr;
		if (!own) {
			return std::nullopt;
		}
		auto*       lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
		const bool  inherit = lightingTemplate && own->lightingTemplateInheritanceFlags.any(RE::INTERIOR_DATA::Inherit::kAmbientColor);
		const auto& dalc = inherit ? lightingTemplate->directionalAmbientLightingColors : own->directionalAmbientLightingColors;

		auto toFloat = [](const RE::Color& a_color, float* a_out) {
			a_out[0] = a_color.red / 255.0f;
			a_out[1] = a_color.green / 255.0f;
			a_out[2] = a_color.blue / 255.0f;
		};
		AmbientState state{};
		const RE::BGSDirectionalAmbientLightingColors::Directional::MaxMin<RE::Color>* axes[3] = { &dalc.directional.x, &dalc.directional.y, &dalc.directional.z };
		for (int axis = 0; axis < 3; ++axis) {
			toFloat(axes[axis]->max, state.colors[axis][0]);
			toFloat(axes[axis]->min, state.colors[axis][1]);
		}
		toFloat(dalc.specular, state.tint);
		state.fresnel = dalc.fresnelPower;
		return state;
	}

	void SetLightDirection(RE::NiLight* a_light, RE::NiPoint3 a_direction)
	{
		// A directional light shines along its local +X.
		a_direction.Unitize();
		RE::NiPoint3 side = a_direction.UnitCross({ 0.0f, 0.0f, 1.0f });
		if (side.Length() < 0.5f) {
			side = { 1.0f, 0.0f, 0.0f };
		}
		const RE::NiPoint3 up = side.UnitCross(a_direction);
		auto&              m = a_light->local.rotate;
		for (int row = 0; row < 3; ++row) {
			m.entry[row][0] = a_direction[row];
			m.entry[row][1] = side[row];
			m.entry[row][2] = up[row];
		}
		RE::NiUpdateData update;
		a_light->Update(update);
	}

	// The engine's directional light (its "sun") for a space, worked out the way Sun::Update
	// (ID 26352) sets it, which it only does for the space the player is in. The light shines along
	// its local +X, so a direction is the first column of the rotation the engine gives it.

	// Interiors (sky in interior mode): rotation Rz(XY) * Ry(Z), the angles in degrees from the
	// cell's own getters (ID 18929 / 18931: its lighting data, or its lighting template's when it
	// inherits the rotation).
	RE::NiPoint3 FormulaDirection(RE::TESObjectCELL* a_cell)
	{
		static REL::Relocation<std::uint32_t (*)(RE::TESObjectCELL*)> rotationXY{ REL::RelocationID(18487, 18929) };
		static REL::Relocation<std::uint32_t (*)(RE::TESObjectCELL*)> rotationZ{ REL::RelocationID(18488, 18931) };
		const float xy = RE::deg_to_rad(static_cast<float>(rotationXY(a_cell)));
		const float z = RE::deg_to_rad(static_cast<float>(rotationZ(a_cell)));
		return { std::cos(xy) * std::cos(z), -std::sin(xy) * std::cos(z), std::sin(z) };
	}

	// Interiors' directional light as the engine set it (the world light scene's sun) while the
	// player was in them, by cell, for this session (CheckSunDirection). Preferred over the formula
	// above, which was measured 89 degrees off in Arcadia's Cauldron: the engine's code builds the
	// light's rotation that way, but something (a node above the light, another mod) turns it.
	std::unordered_map<RE::FormID, RE::NiPoint3> seenDirections;

	RE::NiPoint3 CellDirection(RE::TESObjectCELL* a_cell)
	{
		const auto it = seenDirections.find(a_cell->GetFormID());
		return it != seenDirections.end() ? it->second : FormulaDirection(a_cell);
	}

	// Outdoors (sky in exterior mode): the sun's place on its arc, from the hour against the
	// climate's sunrise and sunset (their middles, widened by half of fSunAlphaTransTime on each
	// side): from east (+1) to west (-1) by day, and back by night. The light travels along
	// (-fSunDirXExtreme x, -fSunDirXExtreme * fSunYExtreme / fSunXExtreme, fSunZExtreme), the
	// settings read where Sun::Update reads them. a_synced: Sky Sync's direction (below), which is
	// the light outdoors; otherwise a recent snapshot is preferred.
	RE::NiPoint3 OutdoorSunDirection(bool* a_synced = nullptr)
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* calendar = RE::Calendar::GetSingleton();
		SEAMLESS_ASSERT(sky && calendar);  // only asked for with a game running
		static REL::Relocation<const float*> alphaTransTime{ REL::RelocationID(502619, 370247) };
		static REL::Relocation<const float*> xExtreme{ REL::RelocationID(502621, 370250) };
		static REL::Relocation<const float*> yExtreme{ REL::RelocationID(502623, 370253) };
		static REL::Relocation<const float*> zExtreme{ REL::RelocationID(502625, 370256) };
		static REL::Relocation<const float*> dirXExtreme{ REL::RelocationID(502627, 370259) };
		// The engine reads the sky's own hour, which may stand still while the sky is in interior
		// mode; the calendar's is the same hour, always current.
		const float hour = calendar->GetHour();
		const float day = RE::Calendar::GetHoursPerDay();
		const float widen = 0.5f * *alphaTransTime;
		const float rise = 0.5f * (sky->GetSunriseBegin() + sky->GetSunriseEnd()) - widen;
		const float set = 0.5f * (sky->GetSunsetBegin() + sky->GetSunsetEnd()) + widen;
		float       x = 0.0f;
		if (hour > rise && hour < set) {
			x = 1.0f - 2.0f * (hour - rise) / (set - rise);
		} else {
			const float intoNight = hour >= set ? hour - set : day - set + hour;
			x = 2.0f * intoNight / (day - (set - rise)) - 1.0f;
		}
		// Community Shaders' Sky Sync turns the light toward the sun's place in the sky (the
		// position Sun::Update gives it, from the same x) instead: ~13 degrees steeper by day than
		// the light above, and the street's shadows seen from indoors fell shorter than outside.
		if (a_synced) {
			*a_synced = false;
		}
		const float position[3] = { x * *xExtreme, *yExtreme, std::abs(*xExtreme) - std::abs(x * *xExtreme) };
		float       towardSun[3]{};
		if (::Render::Backend::OutdoorSunDirection(hour, position, towardSun)) {
			if (a_synced) {
				*a_synced = true;
			}
			return RE::NiPoint3{ -towardSun[0], -towardSun[1], -towardSun[2] };
		}
		RE::NiPoint3 direction{ -*dirXExtreme * x, -*dirXExtreme * *yExtreme / *xExtreme, *zExtreme };
		direction.Unitize();
		return direction;
	}

	// Render thread, before the world is drawn: the two formulas above against the engine's own
	// light in the space the player is in, once per space, so a mistake in them shows in the log.
	// A second after arriving: the engine brings a new space's light in a frame late, and on the
	// frame of arrival the world's sun carries ours (see RenderWorldHook).
	void CheckSunDirection()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
		auto* light = sun ? sun->light.get() : nullptr;
		if (!cell || !light) {
			return;
		}
		static RE::FormID                            current = 0;
		static bool                                  checked = false;
		static std::chrono::steady_clock::time_point since;
		const auto                                   now = std::chrono::steady_clock::now();
		if (cell->GetFormID() != current) {
			current = cell->GetFormID();
			checked = false;
			since = now;
			return;
		}
		if (checked || now - since < std::chrono::seconds(1)) {
			return;
		}
		checked = true;
		const auto&        r = light->world.rotate;
		const RE::NiPoint3 engine{ r.entry[0][0], r.entry[1][0], r.entry[2][0] };
		const auto         ours = cell->IsInteriorCell() ? FormulaDirection(cell) : OutdoorSunDirection();
		if (cell->IsInteriorCell()) {
			seenDirections.insert_or_assign(current, engine);
		}
		const float        apart = RE::rad_to_deg(std::acos(std::clamp(engine.Dot(ours), -1.0f, 1.0f)));
		static int         reported = 0;
		if (apart > 2.0f && reported++ < 20) {
			logger::warn("portal: sun direction in {:08X} is ({:.3f},{:.3f},{:.3f}), computed ({:.3f},{:.3f},{:.3f}): {:.1f} degrees apart", current, engine.x, engine.y,
				engine.z, ours.x, ours.y, ours.z, apart);
		} else if (apart <= 2.0f) {
			static bool interiorConfirmed = false, exteriorConfirmed = false;
			if (!std::exchange(cell->IsInteriorCell() ? interiorConfirmed : exteriorConfirmed, true)) {
				logger::info("portal: computed sun direction matches the engine's {} ({:.2f} degrees apart)", cell->IsInteriorCell() ? "indoors" : "outdoors", apart);
			}
		}
	}

	// The portal light scene's sun is its own NiDirectionalLight (each ShadowSceneNode creates
	// one), so it can carry the cell's directional colour without touching the world's sun.
	void ApplyCellDirectional(RE::ShadowSceneNode* a_scene, RE::TESObjectCELL* a_cell)
	{
		auto* own = a_cell ? a_cell->GetLighting() : nullptr;
		auto* sun = a_scene->GetRuntimeData().sunLight;
		auto* light = sun ? sun->light.get() : nullptr;
		if (!own || !light) {
			return;
		}
		auto*      lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
		const auto pick = [&](RE::INTERIOR_DATA::Inherit a_what) -> const RE::INTERIOR_DATA& {
			return lightingTemplate && own->lightingTemplateInheritanceFlags.any(a_what) ? lightingTemplate->data : *own;
		};
		const auto& color = pick(RE::INTERIOR_DATA::Inherit::kDirectionalColor).directional;
		auto&       runtime = light->GetLightRuntimeData();
		runtime.diffuse = { color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f };
		runtime.fade = pick(RE::INTERIOR_DATA::Inherit::kDirectionalFade).directionalFade;
		SetLightDirection(light, CellDirection(a_cell));
	}

	// Outdoors seen from inside: the engine's sky is in interior mode then, so the exterior is lit
	// from the weather's data for the current hour (sun colour, directional ambient; the sky
	// colours for the background, see DrawSky) instead of from Sky's per-frame state.
	struct SkyColors
	{
		RE::NiColor upper;
		RE::NiColor horizon;
		RE::NiColor lower;
	};

	struct Outdoors
	{
		RE::NiColor  sun;
		AmbientState ambient;
		SkyColors    sky;
		// Distance fog: near and far distance, power, the most it covers, and its two colours.
		float        fogNear = 0.0f;
		float        fogFar = 0.0f;
		float        fogPower = 1.0f;
		float        fogMax = 1.0f;
		RE::NiColor  fogNearColor;
		RE::NiColor  fogFarColor;
	};

	// Blended the way the sky blends them outdoors (Sky::UpdateColors, ID 26232): between two of the
	// weather's four colour sets by the hour, and from the previous weather during a transition.
	// One fixed set per part of the day (the first version) jumped at the boundaries and was far
	// off in between. Indoors the sky still runs this blend for its dome, then overwrites the sun
	// and ambient with the interior's (mode 2), so the weather's values have to be redone here.
	std::optional<Outdoors> WeatherLighting()
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* current = sky ? sky->currentWeather : nullptr;
		if (!current) {
			return std::nullopt;
		}
		auto*                         last = sky->lastWeather;
		RE::Sky::COLOR_BLEND          blend{};
		RE::TESWeather::ColorTime     time1{};
		RE::TESWeather::ColorTime     time2{};
		sky->FillColorBlend(blend, current, sky->currentWeatherPct, time1, time2);
		const auto color = [&](RE::TESWeather::ColorType a_type) {
			RE::Sky::COLOR_BLEND colors = blend;
			sky->FillColorBlendColors(colors, current, last, a_type, time1, time2);
			if (!last) {
				colors.RGBVal[3] = RE::Color();
			}
			RE::NiColor result;
			sky->SetColor(result, &colors, 0.0f);
			return result;
		};
		Outdoors result;
		result.sun = color(RE::TESWeather::ColorTypes::kSunlight);
		result.sky = { color(RE::TESWeather::ColorTypes::kSkyUpper), color(RE::TESWeather::ColorTypes::kHorizon), color(RE::TESWeather::ColorTypes::kSkyLower) };

		// The directional ambient the same way (ID 26252, which also hands it to the renderer, so it
		// isn't called): current weather at the two times, then the previous weather's.
		const auto mix = [&](auto a_get) {
			float out[3] = { 0.0f, 0.0f, 0.0f };
			const auto add = [&](RE::TESWeather* a_weather, RE::TESWeather::ColorTime a_time, float a_weight) {
				const RE::Color& c = a_get(a_weather->directionalAmbientLightingColors[a_time]);
				out[0] += c.red / 255.0f * a_weight;
				out[1] += c.green / 255.0f * a_weight;
				out[2] += c.blue / 255.0f * a_weight;
			};
			add(current, time1, blend.blend[0]);
			add(current, time2, blend.blend[1]);
			if (last) {
				add(last, time1, blend.blend[2]);
				add(last, time2, blend.blend[3]);
			}
			return std::array{ out[0], out[1], out[2] };
		};
		using DALC = RE::BGSDirectionalAmbientLightingColors;
		const auto store = [](const std::array<float, 3>& a_from, float* a_to) { std::copy(a_from.begin(), a_from.end(), a_to); };
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.x.max; }), result.ambient.colors[0][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.x.min; }), result.ambient.colors[0][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.y.max; }), result.ambient.colors[1][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.y.min; }), result.ambient.colors[1][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.z.max; }), result.ambient.colors[2][0]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.directional.z.min; }), result.ambient.colors[2][1]);
		store(mix([](const DALC& d) -> const RE::Color& { return d.specular; }), result.ambient.tint);
		result.ambient.fresnel = current->directionalAmbientLightingColors[time1].fresnelPower * blend.blend[0] +
		                         current->directionalAmbientLightingColors[time2].fresnelPower * blend.blend[1] +
		                         (last ? last->directionalAmbientLightingColors[time1].fresnelPower * blend.blend[2] +
		                                     last->directionalAmbientLightingColors[time2].fresnelPower * blend.blend[3] :
		                                 0.0f);

		// The fog's distances from each weather's day or night values, by the same weights.
		result.fogNearColor = color(RE::TESWeather::ColorTypes::kFogNear);
		result.fogFarColor = color(RE::TESWeather::ColorTypes::kFogFar);
		result.fogNear = result.fogFar = result.fogPower = result.fogMax = 0.0f;
		const auto addFog = [&](RE::TESWeather* a_weather, RE::TESWeather::ColorTime a_time, float a_weight) {
			const auto& fog = a_weather->fogData;
			const bool  night = a_time == RE::TESWeather::ColorTimes::kNight;
			result.fogNear += (night ? fog.nightNear : fog.dayNear) * a_weight;
			result.fogFar += (night ? fog.nightFar : fog.dayFar) * a_weight;
			result.fogPower += (night ? fog.nightPower : fog.dayPower) * a_weight;
			result.fogMax += (night ? fog.nightMax : fog.dayMax) * a_weight;
		};
		addFog(current, time1, blend.blend[0]);
		addFog(current, time2, blend.blend[1]);
		if (last) {
			addFog(last, time1, blend.blend[2]);
			addFog(last, time2, blend.blend[3]);
		}
		return result;
	}

	// The outdoors as the engine itself lit it, the last time the player was out (this very frame
	// while they are): sun colour (with the weather's transitions and the time between its four
	// colour sets) and direction, directional ambient and sky. Seen from indoors shortly after,
	// that is what the street looks like; rebuilt from the weather's tables alone it didn't match
	// (one fixed colour set per part of the day), and the sun's direction worked out the engine's
	// way (OutdoorSunDirection) was measured ~20 degrees off what the engine drew.
	struct OutdoorSnapshot
	{
		RE::NiColor         sun;
		float               sunFade = 1.0f;
		RE::NiPoint3        sunDirection;
		AmbientSnapshot     ambient{};
		SkyColors           sky{};
		float               gameHour = 0.0f;
		RE::TESWorldSpace*  world = nullptr;
		bool                valid = false;
	};
	OutdoorSnapshot outdoorSnapshot;

	// Render thread, before the world is drawn, while the player is outdoors.
	void CaptureOutdoors()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* sky = RE::Sky::GetSingleton();
		if (!cell || cell->IsInteriorCell() || !scene || !sky) {
			return;
		}
		auto* sun = scene->GetRuntimeData().sunLight;
		auto* light = sun ? sun->light.get() : nullptr;
		if (!light) {
			return;
		}
		OutdoorSnapshot snapshot;
		snapshot.sun = light->GetLightRuntimeData().diffuse;
		snapshot.sunFade = light->GetLightRuntimeData().fade;
		const auto& r = light->world.rotate;
		snapshot.sunDirection = { r.entry[0][0], r.entry[1][0], r.entry[2][0] };  // along its local +X
		snapshot.ambient = SaveAmbient();
		using Color = RE::TESWeather::ColorTypes;
		snapshot.sky = { sky->skyColor[Color::kSkyUpper], sky->skyColor[Color::kHorizon], sky->skyColor[Color::kSkyLower] };
		snapshot.gameHour = sky->currentGameHour;
		snapshot.world = cell->GetRuntimeData().worldSpace;
		snapshot.valid = true;
		outdoorSnapshot = snapshot;
	}

	// The snapshot, while it still describes the outdoors of a_cell (same worldspace, under half a
	// game hour old). With the player outdoors it was taken this frame: then it is the outdoors of
	// any worldspace (the far side of a city gate shares the sky, the hour and the weather).
	const OutdoorSnapshot* RecentOutdoors(RE::TESObjectCELL* a_cell)
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		const bool outdoorsNow = here && !here->IsInteriorCell();
		if (!outdoorSnapshot.valid || !sky || !a_cell || (!outdoorsNow && a_cell->GetRuntimeData().worldSpace != outdoorSnapshot.world)) {
			return nullptr;
		}
		float age = sky->currentGameHour - outdoorSnapshot.gameHour;
		if (age < 0.0f) {
			age += 24.0f;
		}
		return age < 0.5f ? &outdoorSnapshot : nullptr;
	}

	// The portal scene's sun and the directional ambient for the remote space; outdoors also the
	// background colour.
	std::optional<SkyColors> ApplyRemoteLighting(RE::ShadowSceneNode* a_scene, RE::TESObjectCELL* a_cell)
	{
		std::optional<AmbientState> ambient;
		std::optional<SkyColors>    background;
		auto*                       sun = a_scene->GetRuntimeData().sunLight;
		auto*                       light = sun ? sun->light.get() : nullptr;
		if (a_cell->IsInteriorCell()) {
			ApplyCellDirectional(a_scene, a_cell);
			ambient = CellAmbient(a_cell);
		} else if (const auto* recent = RecentOutdoors(a_cell)) {
			if (light) {
				// Without the image space's HDR sunlight scale (2.5 outdoors) the world's sun gets: with
				// it, and no shadows on the street, the view out of a dark room was far too bright.
				light->GetLightRuntimeData().diffuse = recent->sun * lightDebug.sunScale;
				light->GetLightRuntimeData().fade = recent->sunFade;
				// As the merged street has it (MergedLighting): Sky Sync's direction moves with the
				// hour, the snapshot stood still, and the shadows jumped at the hand-over to the merge.
				bool       synced = false;
				const auto computed = OutdoorSunDirection(&synced);
				SetLightDirection(light, synced ? computed : recent->sunDirection);
			}
			// The engine's own state, as it was: not through the engine's function again.
			RestoreAmbient(recent->ambient);
			return recent->sky;
		} else if (const auto outdoors = WeatherLighting()) {
			if (light) {
				light->GetLightRuntimeData().diffuse = outdoors->sun;
				light->GetLightRuntimeData().fade = 1.0f;
				SetLightDirection(light, OutdoorSunDirection());
			}
			ambient = outdoors->ambient;
			background = outdoors->sky;
		}
		if (light) {
			auto& diffuse = light->GetLightRuntimeData().diffuse;
			diffuse = diffuse * lightDebug.sunScale;
		}
		if (ambient) {
			for (auto& axis : ambient->colors) {
				for (auto& bound : axis) {
					for (auto& channel : bound) {
						channel *= lightDebug.ambientScale;
					}
				}
			}
			ApplyAmbient(*ambient);
		}
		return background;
	}

	// ---- the sky behind an exterior seen from inside -----------------------------------------
	// The engine's sky isn't drawn in the portal pass (it is in interior mode while the player is
	// inside). A gradient from the weather's sky colours, by the view direction's elevation, fills
	// the background first; the exterior is drawn over it.

	struct SkyPipeline
	{
		ID3D11VertexShader*      vertexShader = nullptr;
		ID3D11PixelShader*       pixelShader = nullptr;
		ID3D11Buffer*            constants = nullptr;
		ID3D11DepthStencilState* depthState = nullptr;
		ID3D11RasterizerState*   rasterState = nullptr;
		ID3D11BlendState*        blendState = nullptr;
	} sky;

	struct alignas(16) SkyConstants
	{
		float forward[4];
		float right[4];  // scaled by the frustum's half width at distance 1
		float up[4];     // scaled by its half height
		float upper[4];
		float horizon[4];
		float lower[4];
		float size[4];
	};

	// Our own shaders and states: one that can't be made is our bug, not a state to draw around.
	void CompileShader(const char* a_source, std::size_t a_size, const char* a_name, const char* a_entry, const char* a_profile, ID3DBlob** a_out)
	{
		ID3DBlob*     errors = nullptr;
		const HRESULT result = ::D3DCompile(a_source, a_size, a_name, nullptr, nullptr, a_entry, a_profile, 0, 0, a_out, &errors);
		if (FAILED(result)) {
			logger::critical("portal: {} {} failed: {}", a_name, a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
		}
		SEAMLESS_ASSERT(SUCCEEDED(result));
		if (errors) {
			errors->Release();
		}
	}

	void EnsureSkyPipeline(ID3D11Device* a_device)
	{
		if (sky.vertexShader) {
			return;
		}
		static const char source[] = R"(
			cbuffer Sky : register(b0) { float4 Forward; float4 Right; float4 Up; float4 Upper; float4 Horizon; float4 Lower; float4 Size; };
			float4 VS(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
			float4 PS(float4 position : SV_Position) : SV_Target {
				float2 ndc = float2(position.x / Size.x * 2 - 1, 1 - position.y / Size.y * 2);
				float3 direction = normalize(Forward.xyz + ndc.x * Right.xyz + ndc.y * Up.xyz);
				float  elevation = direction.z;
				float3 color = elevation >= 0 ? lerp(Horizon.rgb, Upper.rgb, saturate(pow(elevation, 0.6))) : lerp(Horizon.rgb, Lower.rgb, saturate(-elevation * 4));
				return float4(color, 1);
			}
		)";
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		CompileShader(source, sizeof(source) - 1, "SeamlessPortalSky", "VS", "vs_5_0", &vs);
		CompileShader(source, sizeof(source) - 1, "SeamlessPortalSky", "PS", "ps_5_0", &ps);
		D3D11_BUFFER_DESC        buffer{ sizeof(SkyConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_DEPTH_STENCIL_DESC depth{};  // no depth test or write
		D3D11_RASTERIZER_DESC    raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &sky.vertexShader)) &&
		                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &sky.pixelShader)) &&
		                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &sky.constants)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &sky.depthState)) &&
		                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &sky.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &sky.blendState));
		SEAMLESS_ASSERT(made);
		vs->Release();
		ps->Release();
	}

	// Into the top-left a_width x a_height of the target, where the picture is drawn.
	void DrawSky(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, const RE::NiCamera* a_camera, const SkyColors& a_colors, float a_width, float a_height)
	{
		ID3D11Device* device = nullptr;
		a_context->GetDevice(&device);
		EnsureSkyPipeline(device);
		device->Release();

		// NiCamera: looks along local +X, up is +Y, right is +Z.
		const auto& m = a_camera->world.rotate;
		const auto& frustum = a_camera->GetRuntimeData2().viewFrustum;
		SkyConstants c{};
		for (int row = 0; row < 3; ++row) {
			c.forward[row] = m.entry[row][0];
			c.up[row] = m.entry[row][1] * frustum.fTop;
			c.right[row] = m.entry[row][2] * frustum.fRight;
		}
		const auto put = [](float* a_out, const RE::NiColor& a_color) {
			a_out[0] = a_color.red;
			a_out[1] = a_color.green;
			a_out[2] = a_color.blue;
		};
		put(c.upper, a_colors.upper);
		put(c.horizon, a_colors.horizon);
		put(c.lower, a_colors.lower);
		c.size[0] = a_width;
		c.size[1] = a_height;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(sky.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &c, sizeof(c));
		a_context->Unmap(sky.constants, 0);

		// Everything touched here goes back as it was: the engine tracks its own bindings.
		ID3D11RenderTargetView*  savedTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*  savedDepthView = nullptr;
		ID3D11DepthStencilState* savedDepthState = nullptr;
		UINT                     savedStencilRef = 0;
		ID3D11BlendState*        savedBlend = nullptr;
		FLOAT                    savedBlendFactor[4]{};
		UINT                     savedSampleMask = 0;
		ID3D11RasterizerState*   savedRaster = nullptr;
		D3D11_VIEWPORT           savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                     savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11VertexShader*      savedVS = nullptr;
		ID3D11PixelShader*       savedPS = nullptr;
		ID3D11GeometryShader*    savedGS = nullptr;
		ID3D11HullShader*        savedHS = nullptr;
		ID3D11DomainShader*      savedDS = nullptr;
		ID3D11InputLayout*       savedLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
		ID3D11Buffer*            savedConstants = nullptr;
		a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedTargets, &savedDepthView);
		a_context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);
		a_context->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);
		a_context->RSGetState(&savedRaster);
		a_context->RSGetViewports(&savedViewportCount, savedViewports);
		a_context->VSGetShader(&savedVS, nullptr, nullptr);
		a_context->PSGetShader(&savedPS, nullptr, nullptr);
		a_context->GSGetShader(&savedGS, nullptr, nullptr);
		a_context->HSGetShader(&savedHS, nullptr, nullptr);
		a_context->DSGetShader(&savedDS, nullptr, nullptr);
		a_context->IAGetInputLayout(&savedLayout);
		a_context->IAGetPrimitiveTopology(&savedTopology);
		a_context->PSGetConstantBuffers(0, 1, &savedConstants);

		const D3D11_VIEWPORT viewport{ 0, 0, a_width, a_height, 0, 1 };
		a_context->OMSetRenderTargets(1, &a_target, nullptr);
		a_context->OMSetDepthStencilState(sky.depthState, 0);
		a_context->OMSetBlendState(sky.blendState, nullptr, 0xFFFFFFFF);
		a_context->RSSetState(sky.rasterState);
		a_context->RSSetViewports(1, &viewport);
		a_context->VSSetShader(sky.vertexShader, nullptr, 0);
		a_context->PSSetShader(sky.pixelShader, nullptr, 0);
		a_context->GSSetShader(nullptr, nullptr, 0);
		a_context->HSSetShader(nullptr, nullptr, 0);
		a_context->DSSetShader(nullptr, nullptr, 0);
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->PSSetConstantBuffers(0, 1, &sky.constants);
		a_context->Draw(3, 0);

		a_context->PSSetConstantBuffers(0, 1, &savedConstants);
		a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedTargets, savedDepthView);
		a_context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
		a_context->OMSetBlendState(savedBlend, savedBlendFactor, savedSampleMask);
		a_context->RSSetState(savedRaster);
		a_context->RSSetViewports(savedViewportCount, savedViewports);
		a_context->VSSetShader(savedVS, nullptr, 0);
		a_context->PSSetShader(savedPS, nullptr, 0);
		a_context->GSSetShader(savedGS, nullptr, 0);
		a_context->HSSetShader(savedHS, nullptr, 0);
		a_context->DSSetShader(savedDS, nullptr, 0);
		a_context->IASetInputLayout(savedLayout);
		a_context->IASetPrimitiveTopology(savedTopology);
		for (auto* target : savedTargets) {
			if (target) {
				target->Release();
			}
		}
		for (IUnknown* object : std::initializer_list<IUnknown*>{ savedDepthView, savedDepthState, savedBlend, savedRaster, savedVS, savedPS, savedGS, savedHS, savedDS, savedLayout,
				 savedConstants }) {
			if (object) {
				object->Release();
			}
		}
	}

	// ---- the portal pass ----------------------------------------------------------------------
	// Runs in the middle of the engine's frame (right after the world is drawn, see RenderWorldHook),
	// so everything it changes in the engine's render state is put back afterwards.

	// Where the doorway is on screen, as a rectangle in the camera port's 0..1 coordinates
	// (left, right, bottom, top); nothing when it is off screen. The portal is only rendered when
	// the doorway can be seen, and only what is inside this rectangle is culled into it.
	using ScreenRect = std::array<float, 4>;

	// The doorway quad goes behind the panel of a door that swings: the panel swings towards the
	// viewer, so every part of it is then in front of the quad and hides it. In front of the panel,
	// the portal was drawn over the swinging door wherever the panel was still behind the quad
	// (near the hinge, and all of it towards the end of closing). A city gate's leaves are hidden
	// instead of swung (OpenDoor); its quad stays in front of its thick panel, clear of the gate's
	// frame, and as wide and high as the gate's whole mesh: its leaves alone are narrower than the
	// arch they close, and the arch's edges showed the player's own side (a brazier cut in half
	// by the doorway's edge at Whiterun's gate). Where the masonry stands in front of the quad, the
	// depth test keeps it hidden.
	bool IsGate(const Active& a_portal)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		auto* here = door ? door->GetParentCell() : nullptr;
		return here && !here->IsInteriorCell() && a_portal.cell && !a_portal.cell->IsInteriorCell();
	}

	std::array<RE::NiPoint3, 4> DoorwayQuad(const Active& a_portal, const RE::NiPoint3& a_viewer)
	{
		const bool gate = IsGate(a_portal);
		return Portal::Doors::Quad(Portal::Doors::Door(a_portal.sourceDoor), a_viewer, !gate, gate);
	}

	std::optional<ScreenRect> DoorwayOnScreen(const Active& a_portal)
	{
		auto*      camera = RE::Main::WorldRootCamera();
		ScreenRect rect{ 1.0f, 0.0f, 1.0f, 0.0f };
		for (const auto& corner : DoorwayQuad(a_portal, camera->world.translate)) {
			float x = 0, y = 0, z = 0;
			camera->WorldPtToScreenPt3(corner, x, y, z, 1e-5f);
			if (z <= 0.0f) {
				// A corner behind the camera (standing in the doorway): the projection of the rest
				// says nothing reliable; treat the whole screen as the doorway.
				return ScreenRect{ 0.0f, 1.0f, 0.0f, 1.0f };
			}
			rect = { std::min(rect[0], x), std::max(rect[1], x), std::min(rect[2], y), std::max(rect[3], y) };
		}
		if (rect[1] < 0.0f || rect[0] > 1.0f || rect[3] < 0.0f || rect[2] > 1.0f) {
			return std::nullopt;
		}
		constexpr float kMargin = 0.02f;
		return ScreenRect{ std::clamp(rect[0] - kMargin, 0.0f, 1.0f), std::clamp(rect[1] + kMargin, 0.0f, 1.0f), std::clamp(rect[2] - kMargin, 0.0f, 1.0f),
			std::clamp(rect[3] + kMargin, 0.0f, 1.0f) };
	}

	RE::NiPointer<RE::NiCamera> cullCamera;

	// The portal camera's clip distances, for the doorway quad's motion vectors: its near plane is
	// moved up to the door, so its depth values mean other distances than the main view's.
	float portalNear = 15.0f;
	float portalFar = 300000.0f;

	// ---- the portal's own render targets ------------------------------------------------------
	// The engine draws only into the targets of its own table, named by index. The portal pass
	// borrowed two entries, the pause-menu background colour and the world's depth copy. The depth
	// copy is read again later in the frame, so it was saved before the pass and put back after it,
	// and the portal's depth was copied out for the doorway quad: three full-screen depth copies
	// every frame. Instead, textures of our own, made like the engine's, stand in those two table
	// entries for the length of the pass, and the quad reads them directly.
	struct OwnTargets
	{
		RE::BSGraphics::RenderTargetData color{};  // texture, RTV, SRV, UAV (no copy texture)
		RE::BSGraphics::DepthStencilData depth{};
		// The top-left part of the targets the last picture was drawn into, in pixels.
		float pictureWidth = 0.0f;
		float pictureHeight = 0.0f;
	};

	// Two sets: the pass draws into the one not on screen, and the doorway shows the other until
	// the new picture is done (see RenderPortal). A doorway picture's capture draws into the
	// hidden set while the doorway keeps showing the last live picture.
	OwnTargets portalTargetSets[2];
	int        shownTargets = 0;

	OwnTargets& DrawingTargets()
	{
		return portalTargetSets[1 - shownTargets];
	}

	OwnTargets& ShownTargets()
	{
		return portalTargetSets[shownTargets];
	}

	template <class T>
	void ReleaseView(T*& a_object)
	{
		if (a_object) {
			reinterpret_cast<IUnknown*>(a_object)->Release();
			a_object = nullptr;
		}
	}

	void ReleaseDepth(RE::BSGraphics::DepthStencilData& a_depth)
	{
		for (auto& view : a_depth.views) {
			ReleaseView(view);
		}
		for (auto& view : a_depth.readOnlyViews) {
			ReleaseView(view);
		}
		ReleaseView(a_depth.depthSRV);
		ReleaseView(a_depth.stencilSRV);
		ReleaseView(a_depth.texture);
	}

	void ReleaseOwnTargets(OwnTargets& a_set)
	{
		ReleaseView(a_set.color.RTV);
		ReleaseView(a_set.color.SRV);
		ReleaseView(a_set.color.UAV);
		ReleaseView(a_set.color.texture);
		ReleaseDepth(a_set.depth);
	}

	D3D11_TEXTURE2D_DESC DescOf(REX::W32::ID3D11Texture2D* a_texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		reinterpret_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
		return desc;
	}

	bool SameTexture(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
	{
		return a_left.Width == a_right.Width && a_left.Height == a_right.Height && a_left.MipLevels == a_right.MipLevels && a_left.ArraySize == a_right.ArraySize &&
		       a_left.Format == a_right.Format && a_left.SampleDesc.Count == a_right.SampleDesc.Count && a_left.BindFlags == a_right.BindFlags;
	}

	// A view of ours like the engine's view a_like (same description), or none if it has none.
	template <class View, class Desc, class Create>
	void CloneView(REX::W32::ID3D11Texture2D* a_texture, auto* a_like, auto*& a_out, Create a_create)
	{
		if (!a_like) {
			return;
		}
		Desc desc{};
		reinterpret_cast<View*>(a_like)->GetDesc(&desc);
		View* view = nullptr;
		// Identical to a view the engine made of an identical texture: only failing for want of
		// video memory, which the game wouldn't survive either.
		const HRESULT created = a_create(reinterpret_cast<ID3D11Texture2D*>(a_texture), &desc, &view);
		SEAMLESS_ASSERT(SUCCEEDED(created));
		a_out = reinterpret_cast<std::remove_reference_t<decltype(a_out)>>(view);
	}

	// A depth-stencil target of ours made like the engine's a_like: the same texture, the same views
	// (one per slice of an array). Made anew when the engine's has changed.
	void EnsureOwnDepth(RE::BSGraphics::DepthStencilData& a_ours, const RE::BSGraphics::DepthStencilData& a_like)
	{
		SEAMLESS_ASSERT(a_like.texture);
		const auto desc = DescOf(a_like.texture);
		if (a_ours.texture && SameTexture(DescOf(a_ours.texture), desc)) {
			return;
		}
		ReleaseDepth(a_ours);
		ID3D11Device* device = nullptr;
		reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context)->GetDevice(&device);
		ID3D11Texture2D* texture = nullptr;
		const HRESULT    created = device->CreateTexture2D(&desc, nullptr, &texture);
		SEAMLESS_ASSERT(SUCCEEDED(created));
		a_ours.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(texture);
		const auto srv = [&](ID3D11Texture2D* t, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** v) { return device->CreateShaderResourceView(t, d, v); };
		const auto dsv = [&](ID3D11Texture2D* t, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** v) { return device->CreateDepthStencilView(t, d, v); };
		for (std::size_t i = 0; i < std::size(a_like.views); ++i) {
			CloneView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(a_ours.texture, a_like.views[i], a_ours.views[i], dsv);
			CloneView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(a_ours.texture, a_like.readOnlyViews[i], a_ours.readOnlyViews[i], dsv);
		}
		CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_ours.texture, a_like.depthSRV, a_ours.depthSRV, srv);
		CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_ours.texture, a_like.stencilSRV, a_ours.stencilSRV, srv);
		device->Release();
	}

	// Made anew whenever the engine's targets have changed (it recreates them on a resolution change).
	void EnsureOwnTargets(OwnTargets& a_set)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		// The colour is made like the main scene's, not like the pause-menu background whose table
		// entry it takes: that one holds 8 bits per channel, and the world at this point of the
		// frame is still in linear high dynamic range (tone mapped later, with the portal in it).
		// The portal's darks came out in visible bands and its lights clipped.
		const auto& engineColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		const auto& engineDepth = renderer->GetDepthStencilData().depthStencils[kDepthTarget];
		SEAMLESS_ASSERT(engineColor.texture && engineDepth.texture);
		const auto colorDesc = DescOf(engineColor.texture);
		const auto depthDesc = DescOf(engineDepth.texture);
		if (a_set.color.texture && SameTexture(DescOf(a_set.color.texture), colorDesc) && a_set.depth.texture && SameTexture(DescOf(a_set.depth.texture), depthDesc)) {
			return;
		}
		ReleaseView(a_set.color.RTV);
		ReleaseView(a_set.color.SRV);
		ReleaseView(a_set.color.UAV);
		ReleaseView(a_set.color.texture);
		ID3D11Device* device = nullptr;
		reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context)->GetDevice(&device);
		ID3D11Texture2D* colorTexture = nullptr;
		const HRESULT    colorCreated = device->CreateTexture2D(&colorDesc, nullptr, &colorTexture);
		SEAMLESS_ASSERT(SUCCEEDED(colorCreated));
		a_set.color.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(colorTexture);

		const auto rtv = [&](ID3D11Texture2D* t, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** v) { return device->CreateRenderTargetView(t, d, v); };
		const auto srv = [&](ID3D11Texture2D* t, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** v) { return device->CreateShaderResourceView(t, d, v); };
		const auto uav = [&](ID3D11Texture2D* t, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** v) { return device->CreateUnorderedAccessView(t, d, v); };
		CloneView<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(a_set.color.texture, engineColor.RTV, a_set.color.RTV, rtv);
		CloneView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(a_set.color.texture, engineColor.SRV, a_set.color.SRV, srv);
		CloneView<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(a_set.color.texture, engineColor.UAV, a_set.color.UAV, uav);
		device->Release();
		EnsureOwnDepth(a_set.depth, engineDepth);
		logger::info("portal: own render targets made ({}x{} colour format {}, depth format {})", colorDesc.Width, colorDesc.Height, static_cast<int>(colorDesc.Format),
			static_cast<int>(depthDesc.Format));
	}

	void EnsureOwnTargets()
	{
		for (auto& set : portalTargetSets) {
			EnsureOwnTargets(set);
		}
	}

	// While alive, the engine's table names our targets under kColorTarget and kDepthTarget, so
	// everything the engine binds by those indices (the pass's own targets, the clears, effects
	// rebinding "the world's depth target", see WorldDepthTargetHook) is ours. The colour entry's
	// copy texture stays the engine's: nothing in the pass copies the target.
	class OwnTargetsInPlace
	{
	public:
		explicit OwnTargetsInPlace(const OwnTargets& a_set)
		{
			EnsureOwnTargets();
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			color = &renderer->GetRuntimeData().renderTargets[kColorTarget];
			depth = &renderer->GetDepthStencilData().depthStencils[kDepthTarget];
			savedColor = *color;
			savedDepth = *depth;
			color->texture = a_set.color.texture;
			color->RTV = a_set.color.RTV;
			color->SRV = a_set.color.SRV;
			color->UAV = a_set.color.UAV;
			*depth = a_set.depth;
		}
		~OwnTargetsInPlace()
		{
			*color = savedColor;
			*depth = savedDepth;
		}
		OwnTargetsInPlace(const OwnTargetsInPlace&) = delete;
		OwnTargetsInPlace& operator=(const OwnTargetsInPlace&) = delete;

	private:
		RE::BSGraphics::RenderTargetData* color = nullptr;
		RE::BSGraphics::DepthStencilData* depth = nullptr;
		RE::BSGraphics::RenderTargetData  savedColor{};
		RE::BSGraphics::DepthStencilData  savedDepth{};
	};

	// ---- the sun's shadows of an outdoor space behind a door -----------------------------------
	// The portal pass had no sun shadows: through a city gate everything the other side's sun
	// reaches was lit, also what its cliffs and walls shade, and a low sun's specular glinted on the
	// needles of trees standing in a cliff's shadow. The other side's light scene has a sun shadow
	// light of its own (made with the scene, ID 106320); it is run through the engine's sequence for
	// the world's (ID 107132 aims and culls, ID 107137 accumulates, ID 107133 renders), with the
	// portal camera and into shadow map arrays of our own: the world's are still read after the
	// portal pass (Community Shaders keeps a soft copy of them for the rest of the frame).
	RE::BSGraphics::DepthStencilData portalShadowMaps[2];
	constexpr RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL kShadowMapTargets[2]{
		RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM,                   // the cascades (BSShadowDirectionalLight::Render)
		RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM  // drawn too in the engine's usual shadow mode (ID 391108)
	};

	void RenderPortalShadows(RE::ShadowSceneNode* a_root, RE::NiCamera* a_camera)
	{
		auto* light = a_root->GetRuntimeData().sunShadowDirLight;
		auto& table = RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils;
		for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
			EnsureOwnDepth(portalShadowMaps[i], table[kShadowMapTargets[i]]);
		}
		// Cascades fitted to the portal camera, along the sun ApplyRemoteLighting aimed.
		light->UpdateCamera(a_camera);

		// The culling jobs take the scene's top nodes, shared out over one list per job (the world's
		// are made by ID 106323); a light has a culling process per job.
		auto&                                                    lightData = light->GetRuntimeData();
		RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>> jobs;
		jobs.resize(light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray.size());
		std::uint32_t next = 0;
		for (auto& child : a_root->GetChildren()) {
			if (child) {
				jobs[next++ % jobs.size()].push_back(child);
			}
		}
		// The world's culls a last list on its own (from the main culling process); the scene has none.
		RE::BSTArray<RE::NiPointer<RE::NiAVObject>> none;
		for (auto& descriptor : lightData.shadowmapDescriptors) {
			// As the portal's own accumulator (see RenderPortal): passes are made against a light scene.
			descriptor.shaderAccumulator->GetRuntimeData().activeShadowSceneNode = a_root;
		}
		using Cull = void (*)(RE::BSShadowDirectionalLight*, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>&, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>&);
		static REL::Relocation<Cull> cull{ REL::RelocationID(101498, 108492) };
		cull(light, jobs, none);
		std::uint32_t shadowLights = 0;
		light->Accumulate(shadowLights, 0, nullptr);

		RE::BSGraphics::DepthStencilData saved[std::size(kShadowMapTargets)];
		for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
			saved[i] = table[kShadowMapTargets[i]];
			table[kShadowMapTargets[i]] = portalShadowMaps[i];
		}
		std::uint32_t index = 0;
		light->Render(index);
		for (std::size_t i = 0; i < std::size(kShadowMapTargets); ++i) {
			table[kShadowMapTargets[i]] = saved[i];
		}
		for (auto& descriptor : lightData.shadowmapDescriptors) {
			ResetAccumulator(descriptor.shaderAccumulator.get());
		}
		::Render::Backend::SetPortalShadows(light, portalShadowMaps[0].depthSRV, portalShadowMaps[1].depthSRV);
	}

	// Doors standing in the far door's doorway besides the far door itself: a second, ordinary door
	// in the same frame (Riverwood Trader has one; from outside, the merged room's
	// HideDoorsInDoorway hides them). Its back face lies on the clip plane at the far door (see
	// cs.setClip in RenderPortal), so whether it was cut away or drawn changed with the smallest
	// camera movement: a closed door flashed up in the doorway, close to it. Looked for once per
	// far door.
	std::vector<RE::NiAVObject*> DoorsBesideFarDoor(RE::TESObjectREFR* a_linked)
	{
		static RE::FormID              scannedFor = 0;
		static std::vector<RE::FormID> found;
		if (a_linked->GetFormID() != scannedFor) {
			scannedFor = a_linked->GetFormID();
			found.clear();
			const auto center = Portal::Doors::Center(a_linked);
			auto*      cell = a_linked->GetParentCell();
			auto*      world = cell && !cell->IsInteriorCell() ? cell->GetRuntimeData().worldSpace : nullptr;
			const auto visit = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell) {
					return;
				}
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (a_ref != a_linked && base && base->Is(RE::FormType::Door) && a_ref->GetPosition().GetDistance(center) < 120.0f) {
						found.push_back(a_ref->GetFormID());
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};
			visit(cell);
			if (world && world->persistentCell != cell) {
				visit(world->persistentCell);  // load doors are persistent references
			}
			for (const auto id : found) {
				logger::info("portal: door {:08X} stands in the far doorway of {:08X}; left out of the portal pass", id, scannedFor);
			}
		}
		std::vector<RE::NiAVObject*> objects;
		for (const auto id : found) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			if (auto* object = ref ? ref->Get3D() : nullptr) {
				objects.push_back(object);
			}
		}
		return objects;
	}

	// The cells' actors whose 3D carries NiAVObject::kNotVisible get it cleared for a cull; the
	// returned ones are to have it back afterwards, for the engine.
	std::vector<RE::NiAVObject*> ShowActorsForCull(const std::vector<RE::TESObjectCELL*>& a_cells)
	{
		std::vector<RE::NiAVObject*> shown;
		for (auto* cell : a_cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				auto* object = actor ? actor->Get3D() : nullptr;
				if (object && !object->GetAppCulled() && object->GetFlags().any(RE::NiAVObject::Flag::kNotVisible)) {
					object->GetFlags().reset(RE::NiAVObject::Flag::kNotVisible);
					shown.push_back(object);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return shown;
	}

	void HideActorsAgain(const std::vector<RE::NiAVObject*>& a_shown)
	{
		for (auto* object : a_shown) {
			object->GetFlags().set(RE::NiAVObject::Flag::kNotVisible);
		}
	}

	// The doorway picture is of the room without its people: kept for good (and maybe shipped),
	// it showed whoever stood there when it was taken, frozen and blurred, where they no longer
	// were when the real room faded in. The returned ones are to be shown again afterwards.
	std::vector<RE::NiAVObject*> HideActorsForCapture(const std::vector<RE::TESObjectCELL*>& a_cells)
	{
		std::vector<RE::NiAVObject*> hidden;
		for (auto* cell : a_cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				auto* object = actor ? actor->Get3D() : nullptr;
				if (object && object->GetFlags().none(RE::NiAVObject::Flag::kNotVisible)) {
					object->GetFlags().set(RE::NiAVObject::Flag::kNotVisible);
					hidden.push_back(object);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return hidden;
	}

	void ShowActorsAgain(const std::vector<RE::NiAVObject*>& a_hidden)
	{
		for (auto* object : a_hidden) {
			object->GetFlags().reset(RE::NiAVObject::Flag::kNotVisible);
		}
	}

	// A portal pass drawn with the doorway picture's own camera (Portal::Snapshot::CaptureCamera)
	// this frame: its image is in the targets not on screen, with these clip distances.
	struct CaptureDrawn
	{
		RE::FormID door;
		bool       outdoors;  // the space behind the door
		bool       gate;      // IsGate: the doorway is the gate's whole opening (DoorwayQuad)
		float      nearClip;
		float      farClip;
	};
	std::optional<CaptureDrawn> captureDrawn;

	// a_fullTarget: the whole target at its size, unjittered, as a doorway picture's capture is
	// drawn (a diagnostic file wants that too); otherwise the picture for the doorway on screen.
	json RenderPortal(const Active& a_portal, const ScreenRect& a_doorway, const std::optional<Portal::Snapshot::CaptureCamera>& a_capture, bool a_fullTarget)
	{
		auto* root = Portal::LightScene::Get();
		auto* mainCamera = RE::Main::WorldRootCamera();
		if (!mainCamera) {
			throw std::runtime_error("no main camera");
		}
		if (!portalCamera) {
			portalCamera.reset(static_cast<RE::NiCamera*>(mainCamera->Clone()));
		}
		// Frustum, viewport and projection from the main camera every frame: taken once, a change
		// of field of view or aspect (a zoom, the FOV console command, a resized window) put the
		// portal image out of line with the doorway, which is composited pixel for pixel.
		portalCamera->GetRuntimeData2() = mainCamera->GetRuntimeData2();
		defaultNear = portalCamera->GetRuntimeData2().viewFrustum.fNear;
		// A fresh accumulator per remote cell: reusing the previous cell's crashed the batch
		// renderer on its leftover passes. The old one is kept alive, like the old light scenes.
		static std::uint32_t                                          generation = UINT32_MAX;
		static std::vector<RE::NiPointer<RE::BSShaderAccumulator>> retiredAccumulators;
		if (!portalAccumulator || generation != a_portal.generation) {
			if (portalAccumulator) {
				// Kept like the retired light scenes (LightScene::Renew): freeing those crashed, and
				// nothing says the passes cached on shapes don't name their accumulator too.
				retiredAccumulators.push_back(std::move(portalAccumulator));
			}
			portalFrame = 0;
			portalAccumulator.reset(RE::BSShaderAccumulator::Create(0x63));  // engine-wide ctor argument
			generation = a_portal.generation;
		}
		const auto mapping = Portal::Doors::Compute(a_portal);
		const bool capturing = a_capture.has_value();
		portalCamera->local = mapping.toLinked * (capturing ? a_capture->transform : mainCamera->world);
		// A gate's mapping is scaled (Doors::Compute) for where the camera stands; what it sees from
		// there doesn't change with its own scale.
		portalCamera->local.scale = 1.0f;
		if (capturing) {
			auto& frustum = portalCamera->GetRuntimeData2().viewFrustum;
			frustum.fLeft = a_capture->left;
			frustum.fRight = a_capture->right;
			frustum.fBottom = a_capture->bottom;
			frustum.fTop = a_capture->top;
		}
		RE::NiUpdateData update;
		portalCamera->Update(update);

		// Poor man's clip plane: push the near plane up to the linked door, so the walls and the
		// door between the mirrored camera and the doorway don't block the view. Exact only when
		// looking straight at the door (as the capture camera does); an oblique projection would
		// make it exact at any angle.
		auto*              linked = Portal::Doors::Door(a_portal.linkedDoor);
		const auto&        rotate = portalCamera->world.rotate;
		const RE::NiPoint3 forward{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] };  // NiCamera looks along local +X
		// A gate's mapping is scaled: the capture camera stands that much further from the far gate.
		const float        toDoor = capturing ? a_capture->toDoorway * mapping.toLinked.scale : (Portal::Doors::Center(linked) - portalCamera->world.translate).Dot(forward);
		// The capture camera cuts just past the door plane: its
		// picture reaches beyond the doorway, where the room's front wall and the door's frame
		// stood within the 20 units kept for the doorway quad, slivers at the plane that the
		// stand-in drew out into vertical streaks seen from aside. The street's side has its own
		// wall and frame anyway.
		constexpr float kCaptureCut = 12.0f;
		portalCamera->GetRuntimeData2().viewFrustum.fNear = capturing ? toDoor + kCaptureCut : std::max(defaultNear, toDoor - 20.0f);
		// The doorway on screen still shows the last picture, read with the clip distances it was
		// drawn with.
		if (capturing) {
			// In the player's space's units, as the picture keeps its distances: the depth read with
			// them gives the far side's distances divided by the mapping's scale.
			const float scale = mapping.toLinked.scale;
			captureDrawn = CaptureDrawn{ a_capture->door, !a_portal.cell->IsInteriorCell(), IsGate(a_portal), portalCamera->GetRuntimeData2().viewFrustum.fNear / scale,
				portalCamera->GetRuntimeData2().viewFrustum.fFar / scale };
		} else {
			portalNear = portalCamera->GetRuntimeData2().viewFrustum.fNear;
			portalFar = portalCamera->GetRuntimeData2().viewFrustum.fFar;
		}

		// The accumulator draws with the portal's own light scene (the root): with no light scene
		// GetRenderPasses crashed while culling, and borrowing the main view's crashed as soon as
		// the player moved. The eye position is what ID 52727 copies from its camera.
		auto&       data = portalAccumulator->GetRuntimeData();
		const auto& mainData = RE::BSShaderAccumulator::GetMainAccumulator()->GetRuntimeData();
		data.activeShadowSceneNode = root;
		data.renderMode = mainData.renderMode;
		data.eyePosition = portalCamera->world.translate;

		// ---- everything below is restored at the end ----
		auto&       shadowState = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		const auto  savedColorTarget = shadowState.renderTargets[0];
		const auto  savedColorMode = shadowState.setRenderTargetMode[0];
		const auto  savedDepthTarget = shadowState.depthStencil;
		const auto  savedDepthSlice = shadowState.depthStencilSlice;
		const auto  savedDepthTargetMode = shadowState.setDepthStencilMode;
		const auto  savedPosAdjust = shadowState.posAdjust;
		const auto  savedPreviousPosAdjust = shadowState.previousPosAdjust;
		const auto  savedCameraData = shadowState.cameraData;
		auto* const savedAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
		static REL::Relocation<RE::NiCamera**>  currentCamera{ REL::RelocationID(527667, 414580) };  // set by SubmitAccumulator
		static REL::Relocation<std::uint32_t*> depthMode{ REL::RelocationID(524794, 388840) };       // cached DepthStencilDepthMode
		static REL::Relocation<std::uint32_t*> dirtyStates{ REL::RelocationID(524773, 388819) };
		static REL::Relocation<std::uint8_t*>  menuDrawGlobal{ REL::RelocationID(525013, 411494) };
		auto* const savedCamera = *currentCamera;
		const auto  savedDepthMode = *depthMode;
		const auto  savedMenuDrawGlobal = *menuDrawGlobal;
		const auto  mainAmbient = SaveAmbient();
		auto*                   d3dContext = reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
		const OwnTargetsInPlace targets(DrawingTargets());  // to the end of the function, past the restore below

		// The linked door itself sits right at the clip distance; hide it for this pass. Not when it
		// is the one swung open (Manager's OpenDoor): its leaves stand in the space behind, and a
		// city gate's doorway stood empty.
		auto*      linked3D = linked->Get3D();
		const bool wasCulled = linked3D->GetAppCulled();
		portalHiddenShapes.clear();
		if (!Portal::Doors::IsSwung(linked->GetFormID())) {
			linked3D->SetAppCulled(true);
			RE::BSVisit::TraverseScenegraphGeometries(linked3D, [](RE::BSGeometry* a_shape) {
				portalHiddenShapes.insert(a_shape);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		// So are other doors standing in its doorway (see DoorsBesideFarDoor).
		std::vector<std::pair<RE::NiAVObject*, bool>> besideCulled;
		for (auto* object : DoorsBesideFarDoor(linked)) {
			besideCulled.emplace_back(object, object->GetAppCulled());
			object->SetAppCulled(true);
			RE::BSVisit::TraverseScenegraphGeometries(object, [](RE::BSGeometry* a_shape) {
				portalHiddenShapes.insert(a_shape);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		Portal::LightScene::PrepareForView();
		ResetAccumulator(portalAccumulator.get());
		RE::BSCullingProcess::CullingContext context;
		context.accumulator.reset(portalAccumulator.get());
		// Culled with a camera whose frustum only covers the doorway: everything else would be
		// drawn and then hidden by the walls around the door anyway.
		if (!cullCamera) {
			cullCamera.reset(static_cast<RE::NiCamera*>(portalCamera->Clone()));
		}
		cullCamera->local = portalCamera->world;
		{
			cullCamera->GetRuntimeData2() = portalCamera->GetRuntimeData2();
			const auto& full = portalCamera->GetRuntimeData2().viewFrustum;
			auto&       narrow = cullCamera->GetRuntimeData2().viewFrustum;
			// The capture camera's image is all room: none of it is hidden by walls.
			constexpr ScreenRect whole{ 0.0f, 1.0f, 0.0f, 1.0f };
			const auto&          rect = capturing ? whole : a_doorway;
			narrow.fLeft = full.fLeft + (full.fRight - full.fLeft) * rect[0];
			narrow.fRight = full.fLeft + (full.fRight - full.fLeft) * rect[1];
			narrow.fBottom = full.fBottom + (full.fTop - full.fBottom) * rect[2];
			narrow.fTop = full.fBottom + (full.fTop - full.fBottom) * rect[3];
			RE::NiUpdateData cullUpdate;
			cullCamera->Update(cullUpdate);
		}
		context.camera.reset(cullCamera.get());
		context.scene.reset(root);
		// An interior built of rooms (room bounds: Jorrvaskr's living quarters, under rooms all of
		// it) hangs its contents under BSMultiBoundRoom nodes, and a room's cull (ID 75886) walks
		// into its children only when the context's portal graph lists the room, or when the cull
		// ignores multibounds. The portal scene has no graph (see RemoteCell::AttachOne), so such
		// a room drew nothing: a black doorway. Outdoors keeps the normal mode, where a
		// multibound around a group of statics still culls the whole group at once.
		if (a_portal.cell->IsInteriorCell()) {
			context.cullMode = RE::BSCullingProcess::BSCPCullingType::kIgnoreMultiBounds;
		}
		{
			// NPCs of a remote interior keep NiAVObject::kNotVisible on their 3D root, and the cull
			// skips them: in the portal the room was empty. Outdoors the flag was clear.
			const auto actors = capturing ? HideActorsForCapture(Portal::Manager::RemoteCells()) : ShowActorsForCull(Portal::Manager::RemoteCells());
			{
				const Diag::Scope scope("portal cull");
				RE::BSCullingProcess::Process(context);
			}
			if (capturing) {
				ShowActorsAgain(actors);
			} else {
				HideActorsAgain(actors);
			}
		}

		// The other side's sun and ambient, before its shadows are drawn with that sun.
		const auto background = ApplyRemoteLighting(root, a_portal.cell);
		// Diagnostics switch: without the render backend the portal is lit and clipped as the world.
		const bool useBackend = lightDebug.communityShaders;
		const bool shadowed = useBackend && lightDebug.sunShadows && !a_portal.cell->IsInteriorCell() && ::Render::Backend::CanShadowPortal();
		if (shadowed) {
			const Diag::Scope scope("portal shadows");
			Diag::GpuBegin("gpu portal shadows");
			RenderPortalShadows(root, portalCamera.get());
			Diag::GpuEnd("gpu portal shadows");
		}
		if (useBackend) {
			::Render::Backend::SetPortalWorldspace(a_portal.cell->IsInteriorCell() ? nullptr : a_portal.cell->GetRuntimeData().worldSpace);
		}

		// Same sequence as the engine's menu-3D renderer (ID 52727): bind with clear, let the clear
		// happen now (the engine clears lazily, when the binding is applied), then bind without.
		SetColorTarget(0, kColorTarget, RE::BSGraphics::SRTM_CLEAR, true);
		SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_CLEAR, 0);
		{
			static REL::Relocation<void (*)(void*)>                resetState{ REL::RelocationID(75570, 77371) };  // Renderer::ResetState
			static REL::Relocation<void (*)(void*, std::uint32_t)> applyState{ REL::RelocationID(75462, 77247) };
			resetState(RendererObject());
			applyState(RendererObject(), 0);
		}
		SetColorTarget(0, kColorTarget, RE::BSGraphics::SRTM_NO_CLEAR, true);
		SetDepthTarget(kDepthTarget, RE::BSGraphics::SRTM_NO_CLEAR, 0);
		// Binding the targets set the viewport the way the main view's is set, render scale included
		// (ID 77240); the full target's comes with kRenderFlags at the submit.
		const bool atRenderScale = !capturing && !a_fullTarget;
		{
			auto&      drawing = DrawingTargets();
			const auto desc = DescOf(drawing.color.texture);
			const auto scale = atRenderScale ? RenderScale() : std::array{ 1.0f, 1.0f };
			drawing.pictureWidth = desc.Width * scale[0];
			drawing.pictureHeight = desc.Height * scale[1];
		}
		// Start from the normal depth mode instead of whatever the previous pass ended in, and set
		// the global ID 52727 sets around its own draw (the accumulator's draw routine, ID 106583,
		// otherwise may switch to "test EQUAL").
		*depthMode = static_cast<std::uint32_t>(RE::BSGraphics::DepthStencilDepthMode::kTestWrite);
		*dirtyStates |= 0x4;  // DIRTY_DEPTH_MODE
		*menuDrawGlobal = 1;
		// Set by Main::RenderWaterEffects when the main view has water in it; the post-accumulation
		// stage (ID 106584) then rebinds the world's depth buffer for its blended passes, which
		// inside the portal pass would test and draw them against the main view's depth.
		static REL::Relocation<std::uint32_t*> waterMode{ REL::RelocationID(513267, 391008) };
		const auto                             savedWaterMode = *waterMode;
		*waterMode = 0;

		if (background) {
			if (auto* view = reinterpret_cast<ID3D11RenderTargetView*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[kColorTarget].RTV)) {
				DrawSky(d3dContext, view, portalCamera.get(), *background, DrawingTargets().pictureWidth, DrawingTargets().pictureHeight);
			}
		}
		{
			// The far door's plane, facing away from the portal camera: nothing on the camera's side
			// of it can be seen through the doorway (see SeamlessPortalClips in the fork). Its face
			// on the camera's side, so the door's own frame isn't cut.
			RE::NiPoint3 through = mapping.toLinked.rotate * mapping.inward;
			through.z = 0.0f;
			through.Unitize();
			const auto face = Portal::Doors::Center(linked) - through * Portal::Doors::Size(linked).halfDepth;
			farDoorClip = std::pair{ through, face.Dot(through) };
			if (useBackend) {
				const float plane[4] = { through.x, through.y, through.z, face.Dot(through) };
				::Render::Backend::SetPortalClip(plane);
			}
		}
		if (useBackend) {
			auto* sun = root->GetRuntimeData().sunLight;
			::Render::Backend::BeginPortalPass(sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr, a_portal.cell->IsInteriorCell(), portalCamera.get());
		}

		std::vector<std::pair<RE::NiLight*, RE::NiColor>> scaledLights;
		const auto scaleLights = [&](auto& a_lights, float a_scale) {
			for (auto& bsLight : a_lights) {
				if (auto* ni = bsLight ? bsLight->light.get() : nullptr; ni && a_scale != 1.0f) {
					auto& diffuse = ni->GetLightRuntimeData().diffuse;
					scaledLights.emplace_back(ni, diffuse);
					diffuse = diffuse * a_scale;
				}
			}
		};
		scaleLights(root->GetRuntimeData().activeShadowLights, lightDebug.shadowLightScale);
		scaleLights(root->GetRuntimeData().activeLights, lightDebug.pointLightScale);

		portalFrame += 1;
		freedPassesThisSubmit = 0;
		drawnThisSubmit.clear();
		// Only the doorway's part of the picture is ever shown, yet each shape the doorway's cull keeps
		// was shaded across the whole picture: at Solitude's gate 24 times the doorway's pixels. The
		// doorway's rectangle (with its margin) bounds the quad that shows the picture, at render scale
		// and jitter alike. The main view's rain drawn into the picture below is cut the same way. A
		// capture's picture is all of it.
		// Switched off again on leaving, also by an exception (a door unloaded mid-frame, caught in
		// the draw hook): left on, the rest of the frame was cut to the doorway's rectangle.
		struct Scissor
		{
			decltype(shadowState) state;
			bool                  on;
			void Off()
			{
				if (std::exchange(on, false)) {
					state.rasterStateScissorMode = 0;
					state.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE);
				}
			}
			~Scissor() { Off(); }
		} scissor{ shadowState, !capturing && !a_fullTarget };
		if (scissor.on) {
			const float     width = DrawingTargets().pictureWidth, height = DrawingTargets().pictureHeight;
			const D3D11_RECT rect{ static_cast<LONG>(std::floor(a_doorway[0] * width)), static_cast<LONG>(std::floor((1.0f - a_doorway[3]) * height)),
				static_cast<LONG>(std::ceil(a_doorway[1] * width)), static_cast<LONG>(std::ceil((1.0f - a_doorway[2]) * height)) };
			d3dContext->RSSetScissorRects(1, &rect);
			shadowState.rasterStateScissorMode = 1;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE);
		}
		inPortalSubmit = true;
		{
			const Diag::Scope scope("portal submit");
			Diag::GpuBegin("gpu portal submit");
			RE::BSGraphics::Renderer::SubmitAccumulator(portalCamera.get(), portalAccumulator.get(), atRenderScale ? LiveRenderFlags() : kRenderFlags);
			Diag::GpuEnd("gpu portal submit");
		}
		inPortalSubmit = false;
		Diag::AddCount("portal passes", static_cast<double>(drawnThisSubmit.size()));
		// Not for the main view's rain drawn into the picture next (its own pass, see
		// DrawMainPrecipitation); EndPortalPass puts the world's shadows back.
		if (shadowed) {
			::Render::Backend::SetPortalShadows(nullptr, nullptr, nullptr);
		}
		// The weather's rain falls through a city gate too. Not in a doorway picture: it is kept,
		// and would show its streaks standing still in any weather.
		if (useBackend && lightDebug.precipitation && atRenderScale && !a_portal.cell->IsInteriorCell() &&
			!RE::PlayerCharacter::GetSingleton()->GetParentCell()->IsInteriorCell()) {
			DrawMainPrecipitation(root);
		}
		scissor.Off();
		// A capture isn't for the doorway: the last picture stays on screen this frame. Otherwise the
		// new one goes on screen at once; a picture held back for drawing fewer passes than usual
		// stood still in the doorway while the view turned (the doorway's cull is narrowed to its
		// size on screen, so turning away legitimately drew a quarter of the passes).
		if (!capturing) {
			shownTargets = 1 - shownTargets;
		}
		if (portalHiddenDrawn) {
			static std::uint32_t reported = 0;
			if (reported++ < 50) {
				logger::warn("portal: the far door's shapes came up {} time(s) in portal frame {} although culled; left out", portalHiddenDrawn, portalFrame);
			}
			portalHiddenDrawn = 0;
		}
		if (logPassesNext.exchange(false)) {
			logger::info("portal: {} different shapes drawn in this pass", loggedPasses.size());
			for (const auto& [name, count] : loggedPasses) {
				logger::info("portal:   {} x{}", name, count);
			}
			loggedPasses.clear();
		}
		for (auto& [light, diffuse] : scaledLights) {
			light->GetLightRuntimeData().diffuse = diffuse;
		}
		if (useBackend) {
			::Render::Backend::EndPortalPass();
			::Render::Backend::SetPortalClip(nullptr);
		}


		// SubmitAccumulator loads the portal camera into the renderer's camera state, including the
		// "previous frame" matrices TAA and CS reproject with; left there, the whole image ghosted.
		shadowState.posAdjust = savedPosAdjust;
		shadowState.previousPosAdjust = savedPreviousPosAdjust;
		shadowState.cameraData = savedCameraData;
		*currentCamera = savedCamera;
		RE::BSShaderAccumulator::SetCurrentAccumulator(savedAccumulator);
		*menuDrawGlobal = savedMenuDrawGlobal;
		*waterMode = savedWaterMode;
		*depthMode = savedDepthMode;
		*dirtyStates |= 0x4;
		RestoreAmbient(mainAmbient);
		ResetAccumulator(portalAccumulator.get());
		linked3D->SetAppCulled(wasCulled);
		for (auto& [object, culled] : besideCulled) {
			object->SetAppCulled(culled);
		}
		SetColorTarget(0, savedColorTarget, static_cast<std::uint32_t>(savedColorMode), true);
		SetDepthTarget(savedDepthTarget, static_cast<std::uint32_t>(savedDepthTargetMode), savedDepthSlice);
		{
			// Our targets are still bound until the engine next applies its state; the doorway quad
			// reads them as textures, which D3D refuses while they are bound for output.
			ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			d3dContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		}

		// Diagnostics, for the bridge's portal_status: only there in agent sessions, and a walk over
		// the cell's references plus a JSON document every frame is wasted on anyone else.
		if (!Diag::AgentMode()) {
			return json::object();
		}
		const auto& p = portalCamera->world.translate;
		// Where the remote cell's NPCs are in the portal image (0..1, z < 0 = behind).
		json onScreen = json::array();
		a_portal.cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr; actor && actor->Get3D()) {
				float x = 0, y = 0, z = 0;
				portalCamera->WorldPtToScreenPt3(actor->Get3D()->worldBound.center, x, y, z, 1e-5f);
				onScreen.push_back({ { "name", actor->GetDisplayFullName() }, { "screen", { x, y, z } } });
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return {
			{ "actorsOnScreen", onScreen },
			{ "portalCamera", { p.x, p.y, p.z } },
			{ "portalForward", { portalCamera->world.rotate.entry[0][0], portalCamera->world.rotate.entry[1][0], portalCamera->world.rotate.entry[2][0] } },
			{ "mainForward", { mainCamera->world.rotate.entry[0][0], mainCamera->world.rotate.entry[1][0], mainCamera->world.rotate.entry[2][0] } },
			{ "linkedDoorCenter", [&] { const auto c = Portal::Doors::Center(linked); return json{ c.x, c.y, c.z }; }() },
			{ "near", portalCamera->GetRuntimeData2().viewFrustum.fNear },
			{ "worldTargets", { static_cast<std::uint32_t>(savedColorTarget), static_cast<std::uint32_t>(savedColorMode), savedDepthTarget, static_cast<std::uint32_t>(savedDepthTargetMode) } },
		};
	}

	// ---- putting the portal into the world ---------------------------------------------------
	// After the world is drawn and before first person, post-processing and the UI: the doorway
	// quad goes into the scene's own colour target, depth-tested against the scene, showing the
	// portal image pixel for pixel. So the player, the porch and the UI cover it like any other
	// part of the world, and it gets the same tone mapping as everything around it.

	// The fog of the player's space, laid out as the engine uploads it (ID 107320): the world scene's
	// fog property (ID 105637, slot 0) as FogParam, near colour, far colour. The world gets it in the
	// SSAO composite, which has run by the time a doorway's picture goes in, so the picture has to
	// be fogged by itself or it stands out of the haze around it, sharp and dark.
	std::array<float, 12> WorldFog()
	{
		static REL::Relocation<std::byte* (*)(int)> fogOf{ REL::RelocationID(98984, 105637) };
		std::array<float, 12> out{};
		const auto*           property = fogOf(0);
		if (!property) {
			return out;
		}
		const auto read = [&](std::size_t a_offset) {
			float value;
			std::memcpy(&value, property + a_offset, sizeof(value));
			return value;
		};
		const float fogNear = read(0x50), fogFar = read(0x54);
		// The engine uploads a constant for "no fog" when both are 0; a most-it-covers of 0 is that here.
		if (fogFar - fogNear > 1e-3f) {
			out[1] = 1.0f / (fogFar - fogNear);
			out[0] = fogNear * out[1];
			out[2] = read(0x84);
			out[3] = read(0x88);
		}
		for (int i = 0; i < 3; ++i) {
			out[4 + i] = read(0x38 + 4 * i);
			out[8 + i] = read(0x44 + 4 * i);
		}
		return out;
	}

	struct QuadPipeline
	{
		ID3D11VertexShader*      vertexShader = nullptr;
		ID3D11PixelShader*       pixelShader = nullptr;
		ID3D11InputLayout*       layout = nullptr;
		ID3D11Buffer*            vertices = nullptr;
		ID3D11DepthStencilState* depthState = nullptr;
		ID3D11RasterizerState*   rasterState = nullptr;
		ID3D11BlendState*        blendState = nullptr;
		ID3D11Buffer*            motion = nullptr;  // constants for the doorway's motion vectors
	} quad;

	void EnsureQuadPipeline(ID3D11Device* a_device)
	{
		if (quad.vertexShader) {
			return;
		}
		static const char source[] = R"(
			Texture2D<float4> Portal : register(t0);
			Texture2D<float>  PortalDepth : register(t1);
			cbuffer Motion : register(b0)
			{
				row_major float4x4 InverseViewProj;      // of the frame as drawn (jittered)
				row_major float4x4 ViewProj;             // unjittered
				row_major float4x4 PreviousViewProj;     // unjittered, relative to the previous eye
				float4             EyeDelta;             // this frame's eye - the previous frame's; w: portal near
				float4             ViewSize;             // the scene's viewport in pixels; z: see-through; w: portal far
				float4             Debug;                // x: tint the picture red (which flashes are in it)
				float4             Fog;                  // the player's space's, as the engine's FogParam
				float4             FogNear;
				float4             FogFar;
			};
			struct Vertex { float4 position : SV_Position; float2 scale : TEXCOORD0; float depth : TEXCOORD1; };
			struct Pixel { float4 color : SV_Target0; float2 motion : SV_Target1; };
			Vertex VS(float4 position : POSITION, float2 scale : TEXCOORD0) { Vertex v; v.position = position; v.scale = scale; v.depth = position.w; return v; }
			// The composite fogs the world by view depth (not distance); the doorway at its own depth
			// matches the wall around it. What lies behind it was fogged by the portal pass.
			float3 Fogged(float3 color, float depth)
			{
				if (Fog.w <= 0)
					return color;
				float f = min(Fog.w, pow(saturate(depth * Fog.y - Fog.x), Fog.z));
				return lerp(color, lerp(FogNear.rgb, FogFar.rgb, f), f);
			}
			// The scene is drawn into the top-left part of its target when a render scale below 1 is
			// in use (dynamic resolution, upscalers). The live picture is drawn the same way (scale
			// 1); one drawn into its whole target (a diagnostic file) is spread over the scene's part.
			Pixel PS(Vertex v)
			{
				Pixel p;
				int3 texel = int3(v.position.xy * v.scale, 0);
				p.color = float4(Fogged(Portal.Load(texel).rgb, v.depth), 1);
				if (Debug.x > 0.5)
					p.color.gb *= 0.35;
				// What is seen through the doorway moves on screen like static geometry at the
				// portal image's depth (the portal camera is the main camera mapped rigidly).
				float2 ndc = float2(v.position.x / ViewSize.x * 2 - 1, 1 - v.position.y / ViewSize.y * 2);
				// The distance along the view axis from the portal camera's own depth (its near plane
				// is at the door, not where the main view's is), then the point on this pixel's ray.
				float depth = PortalDepth.Load(texel);
				// A city gate: where the portal pass drew nothing (sky, or beyond the few cells it
				// has), the player's own worldspace shows through the doorway. Both worldspaces
				// share their coordinates, distant land and sky, which the portal pass has not.
				if (ViewSize.z > 0.5 && depth >= 1.0)
					discard;
				float viewDistance = EyeDelta.w * ViewSize.w / max(ViewSize.w - depth * (ViewSize.w - EyeDelta.w), 1e-3);
				float4 world = mul(float4(ndc, 0.5, 1), InverseViewProj);
				world /= world.w;
				world.xyz *= viewDistance / mul(world, ViewProj).w;
				float4 now = mul(world, ViewProj);
				float4 before = mul(float4(world.xyz + EyeDelta.xyz, 1), PreviousViewProj);
				p.motion = float2(-0.5, 0.5) * (now.xy / now.w - before.xy / before.w);
				return p;
			}
		)";
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		CompileShader(source, sizeof(source) - 1, "SeamlessPortalQuad", "VS", "vs_5_0", &vs);
		CompileShader(source, sizeof(source) - 1, "SeamlessPortalQuad", "PS", "ps_5_0", &ps);
		const D3D11_INPUT_ELEMENT_DESC elements[]{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		D3D11_BUFFER_DESC        buffer{ sizeof(float) * 6 * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_BUFFER_DESC        motionBuffer{ sizeof(float) * 72, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = TRUE;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &quad.vertexShader)) &&
		                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &quad.pixelShader)) &&
		                  SUCCEEDED(a_device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &quad.layout)) &&
		                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &quad.vertices)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &quad.depthState)) &&
		                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &quad.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &quad.blendState)) &&
		                  SUCCEEDED(a_device->CreateBuffer(&motionBuffer, nullptr, &quad.motion));
		SEAMLESS_ASSERT(made);
		vs->Release();
		ps->Release();
	}

	bool Invert4x4(const double m[16], double out[16])
	{
		double inv[16];
		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
		const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (std::abs(det) < 1e-30) {
			return false;
		}
		for (int i = 0; i < 16; ++i) {
			out[i] = inv[i] / det;
		}
		return true;
	}

	json DrawDoorQuad(const Active& a_portal)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& rendererData = renderer->GetRuntimeData();
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(rendererData.context);
		auto& sceneColor = rendererData.renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& sceneDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& portal = ShownTargets().color;
		auto* sceneRTV = reinterpret_cast<ID3D11RenderTargetView*>(sceneColor.RTV);
		auto* sceneDSV = reinterpret_cast<ID3D11DepthStencilView*>(sceneDepth.views[0]);
		auto* portalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(portal.SRV);
		auto* portalDepthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(ShownTargets().depth.depthSRV);
		SEAMLESS_ASSERT(portalSRV);  // our own target, made like the main scene's (EnsureOwnTargets)
		if (!sceneRTV || !sceneDSV) {
			throw std::runtime_error("scene render target missing");
		}
		ID3D11Device* device = nullptr;
		context->GetDevice(&device);
		EnsureQuadPipeline(device);
		device->Release();

		D3D11_TEXTURE2D_DESC sceneDesc{}, portalDesc{};
		reinterpret_cast<ID3D11Texture2D*>(sceneColor.texture)->GetDesc(&sceneDesc);
		reinterpret_cast<ID3D11Texture2D*>(portal.texture)->GetDesc(&portalDesc);
		if (sceneDesc.Width != portalDesc.Width || sceneDesc.Height != portalDesc.Height) {
			static bool reported = false;
			if (!std::exchange(reported, true)) {
				logger::warn("portal: scene target is {}x{} but the portal image is {}x{}; the portal will be misaligned", sceneDesc.Width, sceneDesc.Height, portalDesc.Width,
					portalDesc.Height);
			}
		}

		// Clip-space corners with the main view's own matrix (camera-relative, row vectors), so the
		// quad sits exactly where the engine drew the door frame, jitter included.
		const auto  eye = mainView.posAdjust.getEye();
		const auto  view = mainView.cameraData.getEye();
		const auto* m = reinterpret_cast<const float*>(&view.viewProjMat);
		const auto  corners = DoorwayQuad(a_portal, RE::Main::WorldRootCamera()->world.translate);
		const auto [scaleX, scaleY] = RenderScale();
		float       clip[4][6];
		for (int i = 0; i < 4; ++i) {
			const float v[3] = { corners[i].x - eye.x, corners[i].y - eye.y, corners[i].z - eye.z };
			for (int column = 0; column < 4; ++column) {
				clip[i][column] = v[0] * m[column] + v[1] * m[4 + column] + v[2] * m[8 + column] + m[12 + column];
			}
			clip[i][4] = ShownTargets().pictureWidth / (sceneDesc.Width * scaleX);
			clip[i][5] = ShownTargets().pictureHeight / (sceneDesc.Height * scaleY);
		}

		// The engine tracks what it has bound; everything touched here goes back exactly as it was.
		struct Saved
		{
			ID3D11RenderTargetView*   targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   depthView = nullptr;
			ID3D11DepthStencilState*  depthState = nullptr;
			UINT                      stencilRef = 0;
			ID3D11BlendState*         blendState = nullptr;
			FLOAT                     blendFactor[4]{};
			UINT                      sampleMask = 0;
			ID3D11RasterizerState*    rasterState = nullptr;
			D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ID3D11VertexShader*       vertexShader = nullptr;
			ID3D11PixelShader*        pixelShader = nullptr;
			ID3D11GeometryShader*     geometryShader = nullptr;
			ID3D11HullShader*         hullShader = nullptr;
			ID3D11DomainShader*       domainShader = nullptr;
			ID3D11InputLayout*        layout = nullptr;
			D3D11_PRIMITIVE_TOPOLOGY  topology{};
			ID3D11Buffer*             vertexBuffer = nullptr;
			UINT                      stride = 0, offset = 0;
			ID3D11ShaderResourceView* texture = nullptr;
			ID3D11ShaderResourceView* texture1 = nullptr;
			ID3D11Buffer*             constants = nullptr;
		} saved;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.targets, &saved.depthView);
		context->OMGetDepthStencilState(&saved.depthState, &saved.stencilRef);
		context->OMGetBlendState(&saved.blendState, saved.blendFactor, &saved.sampleMask);
		context->RSGetState(&saved.rasterState);
		context->RSGetViewports(&saved.viewportCount, saved.viewports);
		context->VSGetShader(&saved.vertexShader, nullptr, nullptr);
		context->PSGetShader(&saved.pixelShader, nullptr, nullptr);
		context->GSGetShader(&saved.geometryShader, nullptr, nullptr);
		context->HSGetShader(&saved.hullShader, nullptr, nullptr);
		context->DSGetShader(&saved.domainShader, nullptr, nullptr);
		context->IAGetInputLayout(&saved.layout);
		context->IAGetPrimitiveTopology(&saved.topology);
		context->IAGetVertexBuffers(0, 1, &saved.vertexBuffer, &saved.stride, &saved.offset);
		context->PSGetShaderResources(0, 1, &saved.texture);
		context->PSGetShaderResources(1, 1, &saved.texture1);
		context->PSGetConstantBuffers(0, 1, &saved.constants);

		// The portal image may still be bound as a render target from the portal pass.
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		const D3D11_VIEWPORT viewport{ 0, 0, sceneDesc.Width * scaleX, sceneDesc.Height * scaleY, 0, 1 };
		const UINT           stride = sizeof(float) * 6, offset = 0;
		// Motion vectors for the doorway, when the portal's depth can be read (the engine's depth
		// copy, which ours is made like, has a view for that).
		auto* motionRTV = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV);
		const bool withMotion = motionRTV && portalDepthSRV && !lightDebug.skipMotion;
		{
			float constants[72]{};
			const auto* current = reinterpret_cast<const float*>(&view.viewProjMat);
			double      a[16], inverse[16];
			for (int i = 0; i < 16; ++i) {
				a[i] = current[i];
			}
			if (withMotion && Invert4x4(a, inverse)) {
				for (int i = 0; i < 16; ++i) {
					constants[i] = static_cast<float>(inverse[i]);
				}
				std::memcpy(constants + 16, &view.viewProjMatrixUnjittered, sizeof(float) * 16);
				std::memcpy(constants + 32, &view.previousViewProjMatrixUnjittered, sizeof(float) * 16);
				const auto previousEye = mainView.previousPosAdjust.getEye();
				constants[48] = eye.x - previousEye.x;
				constants[49] = eye.y - previousEye.y;
				constants[50] = eye.z - previousEye.z;
			}
			constants[51] = portalNear;
			constants[55] = portalFar;
			constants[56] = lightDebug.tintDoorway ? 1.0f : 0.0f;
			auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			constants[54] = portalDepthSRV && here && !here->IsInteriorCell() && a_portal.cell && !a_portal.cell->IsInteriorCell() ? 1.0f : 0.0f;
			constants[52] = sceneDesc.Width * scaleX;
			constants[53] = sceneDesc.Height * scaleY;
			{
				const auto fog = WorldFog();
				std::memcpy(constants + 60, fog.data(), sizeof(float) * 4);
				std::memcpy(constants + 64, fog.data() + 4, sizeof(float) * 3);
				std::memcpy(constants + 68, fog.data() + 8, sizeof(float) * 3);
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(quad.motion, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, constants, sizeof(constants));
				context->Unmap(quad.motion, 0);
			}
		}
		ID3D11RenderTargetView* quadTargets[2] = { sceneRTV, withMotion ? motionRTV : nullptr };
		context->OMSetRenderTargets(withMotion ? 2 : 1, quadTargets, sceneDSV);
		context->PSSetConstantBuffers(0, 1, &quad.motion);
		context->PSSetShaderResources(1, 1, &portalDepthSRV);
		context->OMSetDepthStencilState(quad.depthState, 0);
		context->OMSetBlendState(quad.blendState, nullptr, 0xFFFFFFFF);
		context->RSSetState(quad.rasterState);
		context->RSSetViewports(1, &viewport);
		context->VSSetShader(quad.vertexShader, nullptr, 0);
		context->PSSetShader(quad.pixelShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(quad.layout);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->IASetVertexBuffers(0, 1, &quad.vertices, &stride, &offset);
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(quad.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, clip, sizeof(clip));
				context->Unmap(quad.vertices, 0);
				context->PSSetShaderResources(0, 1, &portalSRV);
				context->Draw(4, 0);
			}
		}

		context->PSSetShaderResources(0, 1, &saved.texture);
		context->PSSetShaderResources(1, 1, &saved.texture1);
		context->PSSetConstantBuffers(0, 1, &saved.constants);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.targets, saved.depthView);
		context->OMSetDepthStencilState(saved.depthState, saved.stencilRef);
		context->OMSetBlendState(saved.blendState, saved.blendFactor, saved.sampleMask);
		context->RSSetState(saved.rasterState);
		context->RSSetViewports(saved.viewportCount, saved.viewports);
		context->VSSetShader(saved.vertexShader, nullptr, 0);
		context->PSSetShader(saved.pixelShader, nullptr, 0);
		context->GSSetShader(saved.geometryShader, nullptr, 0);
		context->HSSetShader(saved.hullShader, nullptr, 0);
		context->DSSetShader(saved.domainShader, nullptr, 0);
		context->IASetInputLayout(saved.layout);
		context->IASetPrimitiveTopology(saved.topology);
		context->IASetVertexBuffers(0, 1, &saved.vertexBuffer, &saved.stride, &saved.offset);
		for (auto* target : saved.targets) {
			if (target) {
				target->Release();
			}
		}
		for (IUnknown* object : std::initializer_list<IUnknown*>{ saved.depthView, saved.depthState, saved.blendState, saved.rasterState, saved.vertexShader, saved.pixelShader,
				 saved.geometryShader, saved.hullShader, saved.domainShader, saved.layout, saved.vertexBuffer, saved.texture, saved.texture1, saved.constants }) {
			if (object) {
				object->Release();
			}
		}
		return { { "composited", true }, { "renderScale", { scaleX, scaleY } }, { "sceneSize", { sceneDesc.Width, sceneDesc.Height } }, { "sceneFormat", static_cast<int>(sceneDesc.Format) },
			{ "portalFormat", static_cast<int>(portalDesc.Format) } };
	}

	// ---- the outdoors' light, seen from inside a place --------------------------------------------

	// ---- doorways before the space behind them is there (Portal::Snapshot) -------------------

	// An interior never seen through its door: a dim grey, a little lighter than a dark room.
	constexpr RE::NiColor kPlainInside{ 0.08f, 0.08f, 0.08f };

	Portal::Snapshot::View SnapshotView()
	{
		const auto             eye = mainView.posAdjust.getEye();
		const auto             previousEye = mainView.previousPosAdjust.getEye();
		const auto             view = mainView.cameraData.getEye();
		Portal::Snapshot::View out;
		std::memcpy(out.viewProj, &view.viewProjMat, sizeof(out.viewProj));
		double matrix[16], inverse[16];
		for (int i = 0; i < 16; ++i) {
			matrix[i] = out.viewProj[i];
		}
		if (Invert4x4(matrix, inverse)) {
			for (int i = 0; i < 16; ++i) {
				out.viewProjInverse[i] = static_cast<float>(inverse[i]);
			}
		}
		std::memcpy(out.viewProjUnjittered, &view.viewProjMatrixUnjittered, sizeof(out.viewProjUnjittered));
		std::memcpy(out.previousViewProjUnjittered, &view.previousViewProjMatrixUnjittered, sizeof(out.previousViewProjUnjittered));
		out.eye = { eye.x, eye.y, eye.z };
		out.eyeDelta = { eye.x - previousEye.x, eye.y - previousEye.y, eye.z - previousEye.z };
		auto&                scene = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC desc{};
		if (scene.texture) {
			reinterpret_cast<ID3D11Texture2D*>(scene.texture)->GetDesc(&desc);
		}
		const auto& graphicsState = RE::BSGraphics::State::GetSingleton()->GetRuntimeData();
		out.width = desc.Width * (graphicsState.dynamicResolutionWidthRatio > 0.0f ? graphicsState.dynamicResolutionWidthRatio : 1.0f);
		out.height = desc.Height * (graphicsState.dynamicResolutionHeightRatio > 0.0f ? graphicsState.dynamicResolutionHeightRatio : 1.0f);
		out.fog = WorldFog();
		return out;
	}

	// After the world (and the portal's picture) is drawn: the doorway shown open on the real space
	// is photographed, and a door swung open before its space is there gets its picture.
	// a_realShown: the real space is in the doorway this frame. A frame without it (the picture
	// not composited) was photographed with the void behind the door, and
	// that picture was kept on disk and shown on every slow load of the door after.
	void DoorwayPictures(bool a_realShown)
	{
		Portal::Snapshot::Flush();
		const auto view = SnapshotView();
		// Before any stand-in picture is drawn into it: the eye adapts to the real view.
		Portal::Snapshot::MeasureScene(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV, view.width, view.height);
		const auto chance = Portal::Manager::SnapshotChance();
		const auto standIns = Portal::Manager::StandIns();
		if (!chance && standIns.empty() && !captureDrawn) {
			return;
		}
		auto*      camera = RE::Main::WorldRootCamera();
		const auto viewer = camera ? camera->world.translate : RE::NiPoint3{};
		// One door at a time: a door that throws (unloaded meanwhile) leaves the others drawn.
		const auto guarded = [](auto&& a_draw) {
			try {
				a_draw();
			} catch (const std::exception& e) {
				static int reported = 0;
				if (reported++ < 10) {
					logger::warn("doorway snapshot: {}", e.what());
				}
			}
		};
		// First person only: in third person the player's own body stands in the doorway.
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		// Merged, the real space is drawn with the world, into the main scene; otherwise by the
		// portal pass, which draws the picture with a camera of its own and needs no first person
		// (in third person the player's body stands in the doorway of the main view only).
		const bool merged = Portal::Manager::Merged();
		if (captureDrawn && Portal::LightOcclusion::UnmeasuredThisFrame() > 0) {
			// Lux's lights not yet known to stand clear of some of the room were half on there; the
			// manager asks again, and the next capture frame measures further.
		} else if (captureDrawn) {
			// Whether or not the doorway is settled (a room waits to be merged for this).
			guarded([&] {
				// The doorway as the picture was asked for with (the snapshot chance's, below): a
				// gate's with another shape was a picture of another size, never the one asked for,
				// and Whiterun's gate was captured every other frame for good, every other frame
				// showing the picture of the frame before (the view through it stuttered, and lagged
				// a turn by a frame half the time).
				const auto                     corners = Portal::Doors::Quad(Portal::Doors::Door(captureDrawn->door), viewer, !captureDrawn->gate, captureDrawn->gate);
				const Portal::Snapshot::Source source{ DrawingTargets().color.SRV, DrawingTargets().depth.depthSRV, captureDrawn->nearClip, captureDrawn->farClip, true };
				Portal::Snapshot::Capture(view, source, captureDrawn->door, captureDrawn->outdoors, captureDrawn->gate, corners);
			});
		} else if (chance && a_realShown && (!merged || (playerCamera && playerCamera->IsInFirstPerson()))) {
			guarded([&] {
				const auto               corners = Portal::Doors::Quad(Portal::Doors::Door(chance->door), viewer, !chance->gate, chance->gate);
				Portal::Snapshot::Source source;
				auto*                    renderer = RE::BSGraphics::Renderer::GetSingleton();
				if (merged) {
					source.color = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV;
					source.depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
					auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
					if (!chance->outdoors && here && !here->IsInteriorCell()) {
						source.lightScale = Portal::Manager::RoomLightScale(chance->door);
					}
				} else {
					source = { ShownTargets().color.SRV, ShownTargets().depth.depthSRV, portalNear, portalFar, false };
				}
				Portal::Snapshot::Capture(view, source, chance->door, chance->outdoors, chance->gate, corners);
			});
		}
		auto        plainOutside = kPlainInside;
		const auto  weather = WeatherLighting();
		float       daylight;
		if (weather) {
			// The horizon's colour for the hour, half way to grey: the sky alone read as a blue
			// panel, a street is mostly walls and ground under it.
			const auto& sky = weather->sky.horizon;
			const float grey = 0.2126f * sky.red + 0.7152f * sky.green + 0.0722f * sky.blue;
			plainOutside = { 0.5f * (sky.red + grey), 0.5f * (sky.green + grey), 0.5f * (sky.blue + grey) };
			// The horizon rather than the sun: under heavy cloud the sun's colour is dim by day, and
			// a street is still far brighter than a room.
			daylight = std::clamp((grey - 0.1f) / 0.3f, 0.0f, 1.0f);
		} else {
			auto*       calendar = RE::Calendar::GetSingleton();
			const float hour = calendar ? calendar->GetHour() : 12.0f;
			daylight = std::clamp(std::min(hour - 5.0f, 21.0f - hour) / 3.0f, 0.0f, 1.0f);
		}
		auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool hereIndoors = here && here->IsInteriorCell();
		for (const auto& standIn : standIns) {
			guarded([&] {
				// Shown: an always-open door of the street merged into a room is drawn moved with it.
				const auto corners = Portal::Doors::ShownQuad(Portal::Doors::Door(standIn.door), viewer, !standIn.gate, standIn.gate);
				using Kind = Portal::Snapshot::Adaptation::Kind;
				Portal::Snapshot::Adaptation adaptation{ Kind::kNone, daylight };
				if (standIn.outdoors && hereIndoors) {
					adaptation.kind = Kind::kOutdoorsFromRoom;
				} else if (!standIn.outdoors && !hereIndoors) {
					adaptation.kind = Kind::kRoomFromOutdoors;
					adaptation.lightScale = Portal::Manager::RoomLightScale(standIn.door);
				}
				Portal::Snapshot::Draw(view, standIn.door, standIn.outdoors, standIn.gate, corners, standIn.alpha, standIn.outdoors ? plainOutside : kPlainInside, adaptation);
			});
		}
	}

	// ---- the frame hook -----------------------------------------------------------------------

	std::optional<Active> PortalToDraw()
	{
		auto portal = Portal::Manager::ActivePortal();
		// Right after an attach the remote cell's references load their 3D over the next frames;
		// until both doors have it there's nothing to map through, which is not a failure (treating
		// it as one made the manager release and re-attach every frame).
		const auto has3D = [](RE::FormID a_id) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			return ref && ref->Get3D();
		};
		if (portal && (!has3D(portal->sourceDoor) || !has3D(portal->linkedDoor))) {
			static int reported = 0;
			if (Portal::Manager::DoorwayOpen() && reported++ < 50) {
				// Diagnostics: with the door already open this is a doorway blinking empty.
				logger::warn("portal: not drawn this frame, a door has no 3D (source {}, linked {})", has3D(portal->sourceDoor), has3D(portal->linkedDoor));
			}
			portal.reset();
		}
		// A door swung shut (walking away, Manager::Update) is a door: the portal's quad on its face
		// would show the other side through it.
		if (portal && !Portal::Manager::DoorwayOpen()) {
			portal.reset();
		}
		return portal;
	}

	// Main::RenderPlayerView as called from ID 36558 (+0x11D): the main view is culled inside it,
	// before Main::RenderWorld. While the camera is at the player's head (walking through a
	// portal doorway, Portal::CameraLimit) the player's body is left out of the main view.
	struct RenderPlayerViewHook
	{
		static void Thunk(void* a_main, bool a_arg2, bool a_arg3)
		{
			Portal::NpcTravel::BeforeRender();
			auto*      body = Portal::CameraLimit::HidePlayer() ? RE::PlayerCharacter::GetSingleton()->Get3D(false) : nullptr;
			const bool wasCulled = body && body->GetAppCulled();
			if (body) {
				body->SetAppCulled(true);
			}
			// Rooms merged beside the shown one are lit from light scenes the engine doesn't update.
			// First: a paused room woken for this view has its people shown below.
			Portal::Manager::BeforePlayerView();
			// NPCs of a merged interior keep NiAVObject::kNotVisible on their 3D root like those of
			// any remote interior (see RenderPortal): the room looked empty from outside, and they
			// faded in on walking in. Cleared for the whole player view (the world is culled before
			// Main::RenderWorld is called), then put back.
			const auto mergedCells = Portal::Manager::MergedCells();
			const auto hiddenActors = !mergedCells.empty() ? ShowActorsForCull(mergedCells) : std::vector<RE::NiAVObject*>{};
			// Here, not in the manager's update: the camera has moved since, and this is the last
			// moment before the engine culls the world.
			Portal::MergedCull::BeforeView();
			{
				const Diag::Scope scope("player view");
				func(a_main, a_arg2, a_arg3);
			}
			Portal::MergedCull::AfterView();
			HideActorsAgain(hiddenActors);
			if (body) {
				body->SetAppCulled(wasCulled);
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The engine decides per actor whether it is drawn (ID 40335, run for every actor with 3D
	// each frame): a frustum test, then a test against the space the camera is in (ID 106433).
	// An actor of a space shown behind a door fails the second one: its 3D gets
	// NiAVObject::kNotVisible and the "drawn" bits of the actor (low nibble of unk274) are cleared.
	// The engine then treats it as off screen: a merged room was empty and its NPCs faded in on
	// walking in, and seen from indoors the people in the street barely animated and had their
	// 3D rebuilt on worker threads while the portal pass drew it (render passes freed under it).
	// For actors of a shown space the result is replaced by "visible".
	struct ActorCullHook
	{
		static void Thunk(void* a_culler, RE::Actor* a_actor)
		{
			original(a_culler, a_actor);
			++calls;
			if (!a_actor || !Portal::Manager::IsShownRemoteCell(a_actor->GetParentCell())) {
				return;
			}
			++hits;
			Portal::Render::ShowActor(a_actor);
		}

		// No call site to redirect (the function is reached through a table), so its first
		// instructions (19 bytes, position independent) are moved to a stub that continues behind
		// them, and the function starts with a jump to the thunk.
		static void Install()
		{
			// The function writes the drawn bits at +0x27C (SE: +0x274); ShowActor writes them through
			// CommonLib's layout, which must agree.
			auto* player = RE::PlayerCharacter::GetSingleton();
			SEAMLESS_ASSERT(reinterpret_cast<std::uintptr_t>(&player->GetActorRuntimeData().unk274) - reinterpret_cast<std::uintptr_t>(player) == REL::Relocate(0x274, 0x27C));
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(39262, 40335) };
			static constexpr std::uint8_t   expected[19] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
			if (std::memcmp(reinterpret_cast<const void*>(target.address()), expected, sizeof(expected)) != 0) {
				logger::warn("portal: actor cull function not as expected; NPCs of a merged room stay invisible");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(sizeof(expected) + 14));
			std::memcpy(stub, expected, sizeof(expected));
			const std::uint8_t jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };  // jmp [rip+0]
			std::memcpy(stub + sizeof(expected), jump, sizeof(jump));
			const std::uintptr_t resume = target.address() + sizeof(expected);
			std::memcpy(stub + sizeof(expected) + sizeof(jump), &resume, sizeof(resume));
			original = reinterpret_cast<decltype(original)>(stub);
			trampoline.write_branch<5>(target.address(), Thunk);
		}
		static inline void (*original)(void*, RE::Actor*) = nullptr;
		static inline std::atomic<std::uint64_t> calls = 0, hits = 0;
	};

	// The engine updates a reference's fade and level of detail every frame (ID 19384, from the cell
	// updates ID 18894 / 18900 and from ID 19204) by its distance from Main::WorldRootCamera: the
	// player's camera. For the street drawn behind a doorway seen from indoors that camera stands
	// in the interior, tens of thousands of units away: the street's buildings were pushed to their
	// farthest level of detail while the portal pass's own cull (ID 105588, from the portal camera)
	// pulled them back, and they were faded out (what RemoteCell forces back in). For whatever
	// hangs under the portal's root this update is left to the portal's cull alone; the reference
	// counts as faded in.
	struct RefFadeHook
	{
		// Its argument is the reference's 3D (every caller passes TESObjectREFR::Get3D2's result),
		// not the reference.
		static bool Thunk(RE::NiAVObject* a_object)
		{
			const RE::NiAVObject* top = a_object;
			while (top->parent) {
				top = top->parent;
			}
			if (Portal::LightScene::IsOurs(top)) {
				return true;
			}
			return func(a_object);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// A fade node's level of detail (ID 105487: level 1..3 by a distance, cross-faded over time
	// between the old and the new level), as called during culls (ID 105486, ID 105588). For
	// whatever hangs under the portal's root the distance is taken as 0: always the nearest, full
	// level, whichever camera the cull measures from. Through a doorway the street is close anyway.
	struct FadeLevelHook
	{
		static void Thunk(RE::BSFadeNode* a_node, float a_distance)
		{
			const RE::NiAVObject* top = a_node;
			while (top && top->parent) {
				top = top->parent;
			}
			func(a_node, Portal::LightScene::IsOurs(top) ? 0.0f : a_distance);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Main::RenderWorld as called from Main::RenderPlayerView (ID 36559 + 0x85E). Community Shaders
	// wraps the same call; installed after it, this wraps both, so the portal pass runs outside
	// CS's deferred "in world" section, like it did when it was drawn at Present.
	struct RenderWorldHook
	{
		static void Thunk(bool a_menuOpen)
		{
			std::optional<FileRequest> file;
			{
				std::scoped_lock lock(stateLock);
				file.swap(pendingFile);
			}
			// The world first: Community Shaders takes its per-frame camera constants from the
			// first pass drawn in a frame. With the portal pass in front, the whole world was
			// composed with the portal camera's matrices (a ghosted double image).
			if (!Portal::Travel::ArrivalFramePending()) {
				CaptureOutdoors();
				CheckSunDirection();
			}
			if (Portal::Travel::ConsumeArrivalFrame()) {
				Diag::MarkArrivalFrame();
				// The engine brings the new space's directional ambient and sun in one frame late
				// (Community Shaders already treats the frame as indoors / outdoors): the first
				// frame after walking in was lit by the full outdoor ambient. Put the new space's
				// lighting in place for this frame; from the next one on the engine's own applies.
				auto* cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				if (cell && world) {
					ApplyRemoteLighting(world, cell);
				}
				// The picture hardly changes across the doorway, but the coordinates do: the camera and
				// the player jump by the distance between the two spaces, and the motion vectors of
				// this frame said so. Frame generation then made the frame between the last one
				// outside and the first one inside out of that jump (one wrong picture on walking
				// through), and the upscaler threw its history away. For this frame the camera and
				// the player count as not having moved.
				{
					auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
					*reinterpret_cast<RE::NiPoint3*>(&shadow.previousPosAdjust) = *reinterpret_cast<const RE::NiPoint3*>(&shadow.posAdjust);
					auto* view = reinterpret_cast<RE::BSGraphics::ViewData*>(&shadow.cameraData);
					view->previousViewProjMatrixUnjittered = view->viewProjMatrixUnjittered;
					auto* player = RE::PlayerCharacter::GetSingleton();
					for (auto* root : { player->Get3D(false), player->Get3D(true) }) {
						if (root) {
							RE::BSVisit::TraverseScenegraphObjects(root, [](RE::NiAVObject* a_object) {
								a_object->previousWorld = a_object->world;
								return RE::BSVisit::BSVisitControl::kContinue;
							});
						}
					}
				}
			}
			{
				const auto& state = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				mainView = { state.posAdjust, state.previousPosAdjust, state.cameraData };
			}
			{
				const Diag::Scope scope("world draw");
				Diag::GpuBegin("gpu world draw");
				func(a_menuOpen);
				Diag::GpuEnd("gpu world draw");
			}
			const Diag::Scope portalScope("portal (all)");
			Diag::GpuBegin("gpu portal (all)");

			auto portal = PortalToDraw();
			if (portal && lightDebug.skipPortalPass) {
				portal.reset();
			}
			json result;
			bool drawn = false;
			bool composited = false;
			captureDrawn.reset();
			if (portal) {
				try {
					// A diagnostic file request wants the whole picture.
					constexpr ScreenRect wholeScreen{ 0.0f, 1.0f, 0.0f, 1.0f };
					// Taken here, so a pass drawn again (below) is the capture camera's too. Not for
					// a diagnostic file, which wants the player's view; asked for again.
					auto capture = Portal::Snapshot::TakeCaptureRequest();
					// Every other frame at most: a room takes several to measure its lights for
					// (see DoorwayPictures), and the doorway on screen stays still while one is drawn.
					static bool capturedLastFrame = false;
					if (const auto door = Portal::Manager::PictureWanted(); !capture && door == portal->sourceDoor && !capturedLastFrame) {
						auto* camera = RE::Main::WorldRootCamera();
						capture = Portal::Snapshot::CameraFor(door, Portal::Doors::Quad(Portal::Doors::Door(door), camera->world.translate, true), camera->world.translate);
					}
					if (capture && (file || capture->door != portal->sourceDoor)) {
						capture.reset();
					}
					capturedLastFrame = capture.has_value();
					// The capture camera doesn't need the doorway on screen.
					const auto doorway = file || capture ? std::optional(wholeScreen) : DoorwayOnScreen(*portal);
					if (doorway) {
						result = RenderPortal(*portal, *doorway, capture, file.has_value());
						// Render passes found freed in the draw were left out (parts of the view
						// missing for that frame: a flicker, seen right after walking in). The cull
						// builds the passes again; drawn once more, the picture is whole.
						if (freedPassesThisSubmit > 0) {
							static std::uint32_t redraws = 0;
							if (redraws++ < 20) {
								logger::info("portal: {} freed render pass(es) skipped; portal pass drawn again", freedPassesThisSubmit);
							}
							result = RenderPortal(*portal, *doorway, capture, file.has_value());
							if (freedPassesThisSubmit > 0 && redraws < 20) {
								logger::warn("portal: the second portal pass still met {} freed render pass(es)", freedPassesThisSubmit);
							}
						}
						result["doorway"] = *doorway;
						drawn = true;
					} else {
						result = { { "offscreen", true } };
					}
				} catch (const std::exception& e) {
					// Doors unloaded mid-frame and similar: drop the portal instead of failing every frame.
					logger::warn("portal: render failed: {}", e.what());
					result = { { "error", e.what() } };
					Portal::Manager::ReleaseAll(std::format("render failed: {}", e.what()));
				}
			}
			if (file) {
				if (drawn) {
					{
						// Saved by table index: the entry must name our image, not the menu background.
						const OwnTargetsInPlace targets(ShownTargets());
						RE::BSGraphics::Renderer::GetSingleton()->SaveRenderTargetToFile(kColorTarget, file->path.c_str(), RE::BSGraphics::TextureFileFormat::kJPG);
					}
					json reply = result;
					reply["path"] = file->path;
					file->done->set_value(reply);
				} else {
					file->done->set_exception(std::make_exception_ptr(std::runtime_error(portal ? result.value("error", "render failed") : "no active portal")));
				}
			}

			if (drawn && !lightDebug.skipComposite) {
				try {
					result.update(DrawDoorQuad(*portal));
					composited = true;
				} catch (const std::exception& e) {
					result["error"] = e.what();
				}
			}
			if (!a_menuOpen) {
				// Merged, the real space is drawn with the world itself. A pass that still met freed
				// render passes has parts of the view missing.
				DoorwayPictures(Portal::Manager::Merged() || (composited && freedPassesThisSubmit == 0));
			}
			if (portal) {
				std::scoped_lock lock(stateLock);
				lastFrame = std::move(result);
			}
			Diag::GpuEnd("gpu portal (all)");
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
}

namespace Portal::Render
{
	void Prewarm()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* context = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
		ID3D11Device* device = nullptr;
		if (context) {
			context->GetDevice(&device);
		}
		if (!device) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		EnsureSkyPipeline(device);
		EnsureQuadPipeline(device);
		device->Release();
		const auto& engineColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		const auto& engineDepth = renderer->GetDepthStencilData().depthStencils[kDepthTarget];
		if (engineColor.texture && engineDepth.texture) {
			EnsureOwnTargets();
		}
		logger::info("portal: shaders and render targets made ahead ({:.1f} ms)", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
	}

	void Install()
	{
		auto& trampoline = SKSE::GetTrampoline();
		REL::Relocation<std::uintptr_t> renderWorld{ REL::RelocationID(35560, 36559), Hooks::Offset(0x831, 0x841, 0x85E) };
		REL::Relocation<std::uintptr_t> drawPass{ REL::RelocationID(100852, 107642), REL::Relocate(0x29E, 0x28F) };
		REL::Relocation<std::uintptr_t> refraction{ REL::RelocationID(99938, 106583), REL::Relocate(0x3C5, 0x3B4) };
		REL::Relocation<std::uintptr_t> worldDepth{ REL::RelocationID(100741, 107522), REL::Relocate(0x6A, 0x6A) };
		// All checked (no short-circuit), so the log names every site that doesn't match.
		const bool sitesFound = static_cast<int>(Hooks::CallsTo(renderWorld.address(), REL::RelocationID(100424, 107142), "portal: Main::RenderWorld")) &
		                        static_cast<int>(Hooks::CallsTo(drawPass.address(), REL::RelocationID(100854, 107644), "portal: SetupAndDrawPass")) &
		                        static_cast<int>(Hooks::CallsTo(refraction.address(), REL::RelocationID(100861, 107655), "portal: refraction passes query")) &
		                        static_cast<int>(Hooks::CallsTo(worldDepth.address(), REL::RelocationID(75650, 77457), "portal: world depth target"));
		if (!sitesFound) {
			logger::error("portal: render hooks not installed (unexpected code at the hook sites); portals stay invisible");
			return;
		}
		RenderWorldHook::func = trampoline.write_call<5>(renderWorld.address(), RenderWorldHook::Thunk);
		ActorCullHook::Install();
		PrecipitationUpdateHook::Install();
		REL::Relocation<std::uintptr_t> playerView{ REL::RelocationID(35559, 36558), REL::Relocate(0x11D, 0x11D) };
		if (Hooks::CallsTo(playerView.address(), REL::RelocationID(35560, 36559), "portal: Main::RenderPlayerView")) {
			RenderPlayerViewHook::func = trampoline.write_call<5>(playerView.address(), RenderPlayerViewHook::Thunk);
		}
		DrawPassHook::func = trampoline.write_call<5>(drawPass.address(), DrawPassHook::Thunk);
		for (const auto& [caller, offset] : { std::pair{ REL::RelocationID(98849, 105486), 0x55 }, std::pair{ REL::RelocationID(98937, 105588), 0x87 } }) {
			REL::Relocation<std::uintptr_t> site{ caller, offset };
			if (Hooks::CallsTo(site.address(), REL::RelocationID(98850, 105487), std::format("portal: fade node level of detail in ID {}", caller.id()))) {
				FadeLevelHook::func = trampoline.write_call<5>(site.address(), FadeLevelHook::Thunk);
			}
		}
		for (const auto& [caller, offset] : { std::pair{ REL::RelocationID(18463, 18894), 0x90 }, std::pair{ REL::RelocationID(18469, 18900), 0xA2 }, std::pair{ REL::RelocationID(18714, 19204), 0x96 } }) {
			REL::Relocation<std::uintptr_t> site{ caller, offset };
			if (Hooks::CallsTo(site.address(), REL::RelocationID(18972, 19384), std::format("portal: reference fade update in ID {}", caller.id()))) {
				// All three call the same function; the thunk's func is that function either way.
				RefFadeHook::func = trampoline.write_call<5>(site.address(), RefFadeHook::Thunk);
			}
		}
		RefractionPassesHook::func = trampoline.write_call<5>(refraction.address(), RefractionPassesHook::Thunk);
		WorldDepthTargetHook::func = trampoline.write_call<5>(worldDepth.address(), WorldDepthTargetHook::Thunk);
		logger::info("portal: render hooks installed");
	}

	json RenderToFile(const std::string& a_path)
	{
		FileRequest request{ a_path, std::make_shared<std::promise<json>>() };
		auto        future = request.done->get_future();
		{
			std::scoped_lock lock(stateLock);
			pendingFile = std::move(request);
		}
		if (future.wait_for(10s) != std::future_status::ready) {
			std::scoped_lock lock(stateLock);
			pendingFile.reset();
			throw std::runtime_error("no frame rendered in time");
		}
		return future.get();
	}

	void ShowActor(RE::Actor* a_actor)
	{
		if (auto* object = a_actor->Get3D()) {
			object->GetFlags().reset(RE::NiAVObject::Flag::kNotVisible);
			// The low nibble holds the drawn bits ID 40335 writes (cleared for actors it treats as
			// off screen); 0x7 is the value this override has used since it was found to work.
			auto& drawn = a_actor->GetActorRuntimeData().unk274;
			drawn = (drawn & ~0xFu) | 0x7u;
		}
	}

	json ActorCullStats()
	{
		return { { "calls", ActorCullHook::calls.load() }, { "merged", ActorCullHook::hits.load() } };
	}

	json LastFrame()
	{
		std::scoped_lock lock(stateLock);
		return lastFrame;
	}

	bool Precipitation()
	{
		return lightDebug.precipitation;
	}


	void SetPrecipitation(bool a_shown)
	{
		lightDebug.precipitation = a_shown;
	}

	bool TintDoorway()
	{
		return lightDebug.tintDoorway;
	}

	DebugSwitches GetDebugSwitches()
	{
		return { lightDebug.skipPortalPass, lightDebug.skipComposite, lightDebug.skipMotion, !lightDebug.communityShaders };
	}

	void SetDebugSwitches(const DebugSwitches& a_switches)
	{
		lightDebug.skipPortalPass = a_switches.skipPortalPass;
		lightDebug.skipComposite = a_switches.skipComposite;
		lightDebug.skipMotion = a_switches.skipMotion;
		lightDebug.communityShaders = !a_switches.skipCommunityShaders;
	}

	void SetTintDoorway(bool a_tint)
	{
		lightDebug.tintDoorway = a_tint;
	}

	json SetLightDebug(const json& a_settings)
	{
		lightDebug.ambientScale = a_settings.value("ambientScale", lightDebug.ambientScale);
		lightDebug.sunScale = a_settings.value("sunScale", lightDebug.sunScale);
		lightDebug.communityShaders = a_settings.value("communityShaders", lightDebug.communityShaders);
		lightDebug.shadowLightScale = a_settings.value("shadowLightScale", lightDebug.shadowLightScale);
		lightDebug.pointLightScale = a_settings.value("pointLightScale", lightDebug.pointLightScale);
		lightDebug.precipitation = a_settings.value("precipitation", lightDebug.precipitation);
		lightDebug.sunShadows = a_settings.value("sunShadows", lightDebug.sunShadows);
		if (a_settings.value("logPasses", false)) {
			logPassesNext = true;
		}
		auto& scene = Portal::LightScene::Get()->GetRuntimeData();
		json  shadowLights = json::array();
		for (auto& bsLight : scene.activeShadowLights) {
			if (auto* ni = bsLight ? bsLight->light.get() : nullptr) {
				const auto& d = ni->GetLightRuntimeData();
				shadowLights.push_back(std::format("'{}' pos=({:.0f},{:.0f},{:.0f}) diffuse=({:.2f},{:.2f},{:.2f}) fade={:.2f} radius={:.0f}", ni->name.c_str(), ni->world.translate.x,
					ni->world.translate.y, ni->world.translate.z, d.diffuse.red, d.diffuse.green, d.diffuse.blue, d.fade, d.radius.x));
			}
		}
		return { { "ambientScale", lightDebug.ambientScale }, { "sunScale", lightDebug.sunScale }, { "communityShaders", lightDebug.communityShaders },
			{ "shadowLightScale", lightDebug.shadowLightScale }, { "pointLightScale", lightDebug.pointLightScale }, { "pointLights", scene.activeLights.size() },
			{ "shadowLights", shadowLights }, { "sunShadows", lightDebug.sunShadows } };
	}

	std::optional<std::array<float, 19>> MergedLighting(RE::TESObjectCELL* a_cell, const RE::NiMatrix3& a_rotation)
	{
		if (a_cell && !a_cell->IsInteriorCell()) {
			// The street merged into an interior: lit as the engine lit it the last time the player
			// was out (the sky is in interior mode now and computes no sun for it).
			// The colours from the weather, worked out as the sky does (equal to the engine's own to
			// 1e-7, compared outdoors): the snapshot alone left the street unlit, as dark as the room
			// in broad daylight, after loading a game indoors or half an hour of game time inside.
			const auto weather = WeatherLighting();
			if (!weather) {
				return std::nullopt;
			}
			const RE::NiTransform outdoorAmbient = AmbientTransform(weather->ambient);
			const RE::NiColor     sun = weather->sun;
			// Sky Sync's direction moves with the hour; the snapshot stands still from the moment
			// the player came in, and is half an hour old at most.
			bool                  synced = false;
			const RE::NiPoint3    computed = OutdoorSunDirection(&synced);
			const auto*           recent = synced ? nullptr : RecentOutdoors(a_cell);
			const RE::NiPoint3    sunDirection = recent ? recent->sunDirection : computed;
			const RE::NiPoint3    towardSun = a_rotation * (sunDirection * -1.0f);
			std::array<float, 19> out{};
			out[18] = 1.0f;
			out[0] = sun.red;
			out[1] = sun.green;
			out[2] = sun.blue;
			out[3] = towardSun.x;
			out[4] = towardSun.y;
			out[5] = towardSun.z;
			const RE::NiMatrix3 m = outdoorAmbient.rotate * a_rotation.Transpose();
			for (int row = 0; row < 3; ++row) {
				out[6 + row * 4] = m.entry[row][0];
				out[7 + row * 4] = m.entry[row][1];
				out[8 + row * 4] = m.entry[row][2];
				out[9 + row * 4] = outdoorAmbient.translate[row];
			}
			return out;
		}
		auto* own = a_cell && a_cell->IsInteriorCell() ? a_cell->GetLighting() : nullptr;
		const auto ambient = CellAmbient(a_cell);
		if (!own || !ambient) {
			return std::nullopt;
		}
		const RE::NiTransform cellAmbient = AmbientTransform(*ambient);

		auto*      lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
		const auto pick = [&](RE::INTERIOR_DATA::Inherit a_what) -> const RE::INTERIOR_DATA& {
			return lightingTemplate && own->lightingTemplateInheritanceFlags.any(a_what) ? lightingTemplate->data : *own;
		};
		const auto& color = pick(RE::INTERIOR_DATA::Inherit::kDirectionalColor).directional;
		const float fade = pick(RE::INTERIOR_DATA::Inherit::kDirectionalFade).directionalFade;
		// Same direction as ApplyCellDirectional (the way the light travels), turned around.
		const RE::NiPoint3 towardLight = a_rotation * -CellDirection(a_cell);

		std::array<float, 19> out{};
		out[18] = 1.0f;
		out[0] = color.red / 255.0f * fade;
		out[1] = color.green / 255.0f * fade;
		out[2] = color.blue / 255.0f * fade;
		out[3] = towardLight.x;
		out[4] = towardLight.y;
		out[5] = towardLight.z;
		// ambient(n_world) = M * n_cell + t with n_cell = R^T n_world, so M' = M R^T.
		const RE::NiMatrix3 m = cellAmbient.rotate * a_rotation.Transpose();
		for (int row = 0; row < 3; ++row) {
			out[6 + row * 4] = m.entry[row][0];
			out[7 + row * 4] = m.entry[row][1];
			out[8 + row * 4] = m.entry[row][2];
			out[9 + row * 4] = cellAmbient.translate[row];
		}
		return out;
	}

	json OutdoorLightingProbe()
	{
		const auto Rgb = [](const RE::NiColor& a_color) { return json::array({ a_color.red, a_color.green, a_color.blue }); };
		const auto Transform = [](const RE::NiTransform& a_t) {
			json rows = json::array();
			for (int row = 0; row < 3; ++row) {
				rows.push_back({ a_t.rotate.entry[row][0], a_t.rotate.entry[row][1], a_t.rotate.entry[row][2], a_t.translate[row] });
			}
			return rows;
		};
		json result;
		if (const auto weather = WeatherLighting()) {
			result["weather"] = { { "sun", Rgb(weather->sun) }, { "ambient", Transform(AmbientTransform(weather->ambient)) }, { "skyUpper", Rgb(weather->sky.upper) },
				{ "fog", { weather->fogNear, weather->fogFar, weather->fogPower, weather->fogMax } }, { "fogNearColor", Rgb(weather->fogNearColor) },
				{ "fogFarColor", Rgb(weather->fogFarColor) } };
			if (auto* sky = RE::Sky::GetSingleton()) {
				result["skyFog"] = { sky->fogNear, sky->fogFar, sky->fogPower, sky->fogClamp };
			}
		}
		if (outdoorSnapshot.valid) {
			RE::NiTransform built;
			std::memcpy(&built, outdoorSnapshot.ambient.built.data(), sizeof(RE::NiTransform));
			result["snapshot"] = { { "sun", Rgb(outdoorSnapshot.sun) }, { "fade", outdoorSnapshot.sunFade }, { "ambient", Transform(built) }, { "hour", outdoorSnapshot.gameHour },
				{ "direction", { outdoorSnapshot.sunDirection.x, outdoorSnapshot.sunDirection.y, outdoorSnapshot.sunDirection.z } } };
		}
		const auto computed = OutdoorSunDirection();
		result["computedDirection"] = { computed.x, computed.y, computed.z };
		return result;
	}

	std::optional<RE::NiColor> OutdoorAmbient(const RE::NiPoint3& a_normal)
	{
		const auto weather = WeatherLighting();
		if (!weather) {
			return std::nullopt;
		}
		const RE::NiTransform ambient = AmbientTransform(weather->ambient);
		const RE::NiPoint3    color = ambient.rotate * a_normal + ambient.translate;
		return RE::NiColor{ std::max(color.x, 0.0f), std::max(color.y, 0.0f), std::max(color.z, 0.0f) };
	}

	std::optional<std::array<float, 10>> MergedFog()
	{
		const auto weather = WeatherLighting();
		if (!weather || weather->fogFar <= weather->fogNear) {
			return std::nullopt;
		}
		const auto& w = *weather;
		return std::array<float, 10>{ w.fogNear, w.fogFar, w.fogPower, w.fogMax, w.fogNearColor.red, w.fogNearColor.green, w.fogNearColor.blue, w.fogFarColor.red,
			w.fogFarColor.green, w.fogFarColor.blue };
	}

	std::optional<std::array<float, 10>> CellFog(RE::TESObjectCELL* a_cell)
	{
		auto* own = a_cell && a_cell->IsInteriorCell() ? a_cell->GetLighting() : nullptr;
		if (!own) {
			return std::nullopt;
		}
		auto*      lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
		const auto pick = [&](RE::INTERIOR_DATA::Inherit a_what) -> const RE::INTERIOR_DATA& {
			return lightingTemplate && own->lightingTemplateInheritanceFlags.any(a_what) ? lightingTemplate->data : *own;
		};
		using Inherit = RE::INTERIOR_DATA::Inherit;
		const float fogNear = pick(Inherit::kFogNear).fogNear;
		const float fogFar = pick(Inherit::kFogFar).fogFar;
		const auto& colors = pick(Inherit::kFogColor);
		// No fog at all (the engine's own "no fog" when both distances are 0): a most-it-covers of 0.
		const float most = fogFar > fogNear ? pick(Inherit::kFogMax).fogClamp : 0.0f;
		return std::array<float, 10>{ fogNear, std::max(fogFar, fogNear + 1.0f), pick(Inherit::kFogPower).fogPower, most, colors.fogColorNear.red / 255.0f,
			colors.fogColorNear.green / 255.0f, colors.fogColorNear.blue / 255.0f, colors.fogColorFar.red / 255.0f, colors.fogColorFar.green / 255.0f,
			colors.fogColorFar.blue / 255.0f };
	}
}
