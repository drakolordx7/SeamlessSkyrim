#pragma once

#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/Active.h"

#include <d3d11.h>

// The portal pass: the remote cell drawn from the main camera mirrored through the door pair (same
// cull -> submit sequence as the engine's menu-3D renderer, ID 52727), into render targets of our
// own, in the middle of the engine's frame. Everything it changes in the engine's render state is
// put back afterwards.
namespace Render::PortalPass
{
	// The engine draws only into the targets of its own table, named by index. The portal pass
	// takes over two entries, the pause-menu background colour and the world's depth copy, with
	// textures of our own made like the engine's (the colour like the main scene's), for the length
	// of the pass; the doorway quad reads them directly.
	inline constexpr auto kColorTarget = RE::RENDER_TARGETS::kMENUBG;
	inline constexpr auto kDepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY;

	struct OwnTargets
	{
		RE::BSGraphics::RenderTargetData color{};  // texture, RTV, SRV, UAV (no copy texture)
		RE::BSGraphics::DepthStencilData depth{};
		// The top-left part of the targets the last picture was drawn into, in pixels.
		float pictureWidth = 0.0f;
		float pictureHeight = 0.0f;
	};

	// Two sets: the pass draws into the one not on screen, and the doorway shows the other until
	// the new picture is done. A doorway picture's capture draws into the hidden set while the
	// doorway keeps showing the last live picture.
	[[nodiscard]] OwnTargets& DrawingTargets();
	[[nodiscard]] OwnTargets& ShownTargets();
	void                      EnsureOwnTargets();

	// While alive, the engine's table names a set's targets under kColorTarget and kDepthTarget, so
	// everything the engine binds by those indices (the pass's own targets, the clears, effects
	// rebinding "the world's depth target") is ours. The colour entry's copy texture stays the
	// engine's: nothing in the pass copies the target.
	class OwnTargetsInPlace
	{
	public:
		explicit OwnTargetsInPlace(const OwnTargets& a_set);
		~OwnTargetsInPlace();
		OwnTargetsInPlace(const OwnTargetsInPlace&) = delete;
		OwnTargetsInPlace& operator=(const OwnTargetsInPlace&) = delete;

	private:
		RE::BSGraphics::RenderTargetData* color = nullptr;
		RE::BSGraphics::DepthStencilData* depth = nullptr;
		RE::BSGraphics::RenderTargetData  savedColor{};
		RE::BSGraphics::DepthStencilData  savedDepth{};
	};

	// Where the doorway is on screen, as a rectangle in the camera port's 0..1 coordinates (left,
	// right, bottom, top); nothing when it is off screen. The portal is only rendered when the
	// doorway can be seen, and only what is inside this rectangle is culled into it.
	using ScreenRect = std::array<float, 4>;
	[[nodiscard]] std::optional<ScreenRect> DoorwayOnScreen(const Portal::Active& a_portal);

	[[nodiscard]] std::array<RE::NiPoint3, 4> DoorwayQuad(const Portal::Active& a_portal, const RE::NiPoint3& a_viewer);

	// A portal pass drawn with the doorway picture's own camera (Pictures::CaptureCamera)
	// this frame: its image is in the targets not on screen, with these clip distances.
	struct CaptureDrawn
	{
		RE::FormID door;
		bool       outdoors;  // the space behind the door
		bool       gate;      // Gates::IsGate: the doorway is the gate's whole opening (DoorwayQuad)
		float      nearClip;
		float      farClip;
	};
	[[nodiscard]] const std::optional<CaptureDrawn>& CaptureDrawnThisFrame();
	// Render thread, before this frame's pass is drawn.
	void BeginFrame();

	// The clip distances of the portal pass drawn for the doorway on screen: its near plane is moved
	// up to the door, so its depth values mean other distances than the main view's.
	[[nodiscard]] float NearClip();
	[[nodiscard]] float FarClip();

	// Render passes the last pass found freed and left out.
	[[nodiscard]] std::uint32_t FreedPassesThisSubmit();

	// a_fullTarget: the whole target at its size, unjittered, as a doorway picture's capture is
	// drawn (a diagnostic file wants that too); otherwise the picture for the doorway on screen.
	// What it did, for portal_status (empty outside agent sessions).
	json Draw(const Portal::Active& a_portal, const ScreenRect& a_doorway, const std::optional<Pictures::CaptureCamera>& a_capture, bool a_fullTarget);

	// The cells' actors whose 3D carries NiAVObject::kNotVisible get it cleared for a cull; the
	// returned ones are to have it back afterwards, for the engine.
	[[nodiscard]] std::vector<RE::NiAVObject*> ShowActorsForCull(const std::vector<RE::TESObjectCELL*>& a_cells);
	void                                       HideActorsAgain(const std::vector<RE::NiAVObject*>& a_shown);

	// The call sites of the hooks that keep the pass clean (Frame::Install checks them all first).
	// SetupAndDrawPass in the batch renderer; the accumulator's refraction query; the effect shaders'
	// "world depth target" query.
	void HookDrawPass(std::uintptr_t a_site);
	void HookRefractionQuery(std::uintptr_t a_site);
	void HookWorldDepthTarget(std::uintptr_t a_site);

	// The sky's pipeline made ahead.
	void Prewarm(ID3D11Device* a_device);
}
