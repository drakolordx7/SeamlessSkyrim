#include "Skyrim/Render/Frame.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Pictures/Composite.h"
#include "Skyrim/Pictures/DoorwayPictures.h"
#include "Skyrim/Portal/Ahead.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalDoor.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/MainView.h"
#include "Skyrim/Render/PortalPass.h"
#include "Skyrim/Render/SpaceLighting.h"

namespace Render::Frame
{
	namespace
	{
		using Portal::Active;

		struct FileRequest
		{
			std::string                         path;
			std::shared_ptr<std::promise<json>> done;
		};

		std::mutex                 stateLock;
		std::optional<FileRequest> pendingFile;
		json                       lastFrame;

		// Redirects a function that is reached through a table or from several places, so there is no
		// call site to hook: its first a_moved bytes (whole instructions, position independent) are
		// moved to a stub that continues behind them, and the function starts with a jump to
		// a_thunk. The stub, which runs the original.
		void* MovePrologue(std::uintptr_t a_target, std::size_t a_moved, std::uintptr_t a_thunk)
		{
			auto& trampoline = SKSE::GetTrampoline();
			auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(a_moved + 14));
			std::memcpy(stub, reinterpret_cast<const void*>(a_target), a_moved);
			const std::uint8_t jump[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };  // jmp [rip+0]
			std::memcpy(stub + a_moved, jump, sizeof(jump));
			const std::uintptr_t resume = a_target + a_moved;
			std::memcpy(stub + a_moved + sizeof(jump), &resume, sizeof(resume));
			trampoline.write_branch<5>(a_target, a_thunk);
			return stub;
		}

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
				if (!Portal::PortalDoor::Shut() && reported++ < 50) {
					// Diagnostics: with the door already open this is a doorway blinking empty.
					logger::warn("portal: not drawn this frame, a door has no 3D (source {}, linked {})", has3D(portal->sourceDoor), has3D(portal->linkedDoor));
				}
				portal.reset();
			}
			// A door swung shut (walking away, Manager::Update) is a door: the portal's quad on its face
			// would show the other side through it.
			if (portal && Portal::PortalDoor::Shut()) {
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
				// NPCs of a merged interior keep NiAVObject::kNotVisible on their 3D root like those of
				// any remote interior (see PortalPass::Draw): the room looked empty from outside, and they
				// faded in on walking in. Cleared for the whole player view (the world is culled before
				// Main::RenderWorld is called), then put back.
				const auto mergedCells = Portal::MergedView::MergedCells();
				const auto hiddenActors = !mergedCells.empty() ? PortalPass::ShowActorsForCull(mergedCells) : std::vector<RE::NiAVObject*>{};
				// Here, not in the manager's update: the camera has moved since, and this is the last
				// moment before the engine culls the world.
				Portal::MergedCull::BeforeView();
				{
					const Diag::Scope scope("player view");
					func(a_main, a_arg2, a_arg3);
				}
				Portal::MergedCull::AfterView();
				PortalPass::HideActorsAgain(hiddenActors);
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
				if (!a_actor || !Portal::MergedView::IsShownCell(a_actor->GetParentCell())) {
					return;
				}
				++hits;
				ShowActor(a_actor);
			}

			// No call site to redirect (the function is reached through a table), so its first
			// instructions (19 bytes) are moved aside.
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
				original = reinterpret_cast<decltype(original)>(MovePrologue(target.address(), sizeof(expected), reinterpret_cast<std::uintptr_t>(&Thunk)));
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

		// The engine brings the new space's directional ambient and sun in one frame late (Community
		// Shaders already treats the frame as indoors / outdoors): the first frame after walking in
		// was lit by the full outdoor ambient. Put the new space's lighting in place for this frame;
		// from the next one on the engine's own applies.
		void LightArrivalFrame()
		{
			auto* cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (cell && world) {
				SpaceLighting::ApplyRemoteLighting(world, cell);
			}
		}

		// The picture hardly changes across the doorway, but the coordinates do: the camera and the
		// player jump by the distance between the two spaces, and the motion vectors of this frame
		// said so. Frame generation then made the frame between the last one outside and the first
		// one inside out of that jump (one wrong picture on walking through), and the upscaler threw
		// its history away. For this frame the camera and the player count as not having moved.
		void ZeroArrivalMotion()
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

		// The portal pass for a_portal: the doorway on screen, or the whole picture for a diagnostic file
		// or a capture camera. What it did, for portal_status; a_drawn: a picture was drawn.
		json DrawPortal(const Active& a_portal, bool a_forFile, bool& a_drawn)
		{
			try {
				auto capture = Pictures::CaptureCameraFor(a_portal, a_forFile);
				constexpr PortalPass::ScreenRect wholeScreen{ 0.0f, 1.0f, 0.0f, 1.0f };
				// The capture camera doesn't need the doorway on screen.
				const auto doorway = a_forFile || capture ? std::optional(wholeScreen) : PortalPass::DoorwayOnScreen(a_portal);
				if (!doorway) {
					return { { "offscreen", true } };
				}
				json result = PortalPass::Draw(a_portal, *doorway, capture, a_forFile);
				// Render passes found freed in the draw were left out (parts of the view missing for
				// that frame: a flicker, seen right after walking in). The cull builds the passes
				// again; drawn once more, the picture is whole.
				if (PortalPass::FreedPassesThisSubmit() > 0) {
					static std::uint32_t redraws = 0;
					if (redraws++ < 20) {
						logger::info("portal: {} freed render pass(es) skipped; portal pass drawn again", PortalPass::FreedPassesThisSubmit());
					}
					result = PortalPass::Draw(a_portal, *doorway, capture, a_forFile);
					if (PortalPass::FreedPassesThisSubmit() > 0 && redraws < 20) {
						logger::warn("portal: the second portal pass still met {} freed render pass(es)", PortalPass::FreedPassesThisSubmit());
					}
				}
				result["doorway"] = *doorway;
				a_drawn = true;
				return result;
			} catch (const std::exception& e) {
				// Doors unloaded mid-frame and similar: drop the portal instead of failing every frame.
				logger::warn("portal: render failed: {}", e.what());
				Portal::Manager::ReleaseAll(std::format("render failed: {}", e.what()));
				return { { "error", e.what() } };
			}
		}

		// Answers a diagnostic file request: the portal's picture saved, or why not.
		void AnswerFileRequest(FileRequest& a_request, bool a_hadPortal, bool a_drawn, const json& a_result)
		{
			if (!a_drawn) {
				a_request.done->set_exception(std::make_exception_ptr(std::runtime_error(a_hadPortal ? a_result.value("error", "render failed") : "no active portal")));
				return;
			}
			{
				// Saved by table index: the entry must name our image, not the menu background.
				const PortalPass::OwnTargetsInPlace targets(PortalPass::ShownTargets());
				RE::BSGraphics::Renderer::GetSingleton()->SaveRenderTargetToFile(PortalPass::kColorTarget, a_request.path.c_str(), RE::BSGraphics::TextureFileFormat::kJPG);
			}
			json reply = a_result;
			reply["path"] = a_request.path;
			a_request.done->set_value(reply);
		}

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
					SpaceLighting::CaptureOutdoors();
					SunDirections::Observe();
				}
				if (Portal::Travel::ConsumeArrivalFrame()) {
					Diag::MarkArrivalFrame();
					LightArrivalFrame();
					ZeroArrivalMotion();
				}
				CaptureMainView();
				{
					const Diag::Scope scope("world draw");
					Diag::GpuBegin("gpu world draw");
					func(a_menuOpen);
					Diag::GpuEnd("gpu world draw");
				}
				const Diag::Scope portalScope("portal (all)");
				Diag::GpuBegin("gpu portal (all)");

				auto portal = PortalToDraw();
				json result;
				bool drawn = false;
				PortalPass::BeginFrame();
				if (portal) {
					result = DrawPortal(*portal, file.has_value(), drawn);
				}
				if (file) {
					AnswerFileRequest(*file, portal.has_value(), drawn, result);
				}
				bool composited = false;
				if (drawn) {
					try {
						result.update(Pictures::Composite::Draw(*portal));
						composited = true;
					} catch (const std::exception& e) {
						result["error"] = e.what();
					}
				}
				if (!a_menuOpen) {
					// Merged, the real space is drawn with the world itself. A pass that still met freed
					// render passes has parts of the view missing. Said before the pass for a picture of
					// a space out of sight, which counts its own.
					const bool realShown = Portal::Manager::Merged() || (composited && PortalPass::FreedPassesThisSubmit() == 0);
					// After the shown portal's pass and before what was drawn with a capture camera is taken.
					Portal::Ahead::DrawPass();
					Pictures::UpdateDoorwayPictures(realShown);
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

	void Prewarm()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* device = D3D::Device();
		const auto started = std::chrono::steady_clock::now();
		PortalPass::Prewarm(device);
		Pictures::Composite::Prewarm(device);
		const auto& engineColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		const auto& engineDepth = renderer->GetDepthStencilData().depthStencils[PortalPass::kDepthTarget];
		if (engineColor.texture && engineDepth.texture) {
			PortalPass::EnsureOwnTargets();
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
		REL::Relocation<std::uintptr_t> playerView{ REL::RelocationID(35559, 36558), REL::Relocate(0x11D, 0x11D) };
		if (Hooks::CallsTo(playerView.address(), REL::RelocationID(35560, 36559), "portal: Main::RenderPlayerView")) {
			RenderPlayerViewHook::func = trampoline.write_call<5>(playerView.address(), RenderPlayerViewHook::Thunk);
		}
		PortalPass::HookDrawPass(drawPass.address());
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
		PortalPass::HookRefractionQuery(refraction.address());
		PortalPass::HookWorldDepthTarget(worldDepth.address());
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
}
