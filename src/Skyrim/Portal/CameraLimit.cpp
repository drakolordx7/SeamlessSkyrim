#include "Skyrim/Portal/CameraLimit.h"

#include "Skyrim/Portal/Pristine.h"

namespace
{
	// Below this horizontal distance the camera is inside or right at the player's head, so the
	// player's body is hidden (like first person) instead of being drawn from the inside.
	constexpr float kHidePlayerBelow = 45.0f;

	std::optional<float> limit;
	bool                 playerHidden = false;
	// Shapes hidden here, to be shown again. Culling the root (as RenderPlayerViewHook does) hid
	// the body but left the head and hair drawn, so every shape is hidden on its own.
	std::vector<RE::NiPointer<RE::BSGeometry>> hiddenShapes;

	void SetHidden(bool a_hide)
	{
		playerHidden = a_hide;
		if (!a_hide) {
			for (auto& shape : hiddenShapes) {
				Portal::Pristine::Show(shape.get());
			}
			hiddenShapes.clear();
			return;
		}
		auto* body = RE::PlayerCharacter::GetSingleton()->Get3D(false);
		if (!body || !hiddenShapes.empty()) {
			return;
		}
		RE::BSVisit::TraverseScenegraphGeometries(body, [](RE::BSGeometry* a_shape) {
			// Through Pristine, as every shape hidden for longer than a view: a model loaded meanwhile
			// may be a copy of it.
			if (!a_shape->GetAppCulled()) {
				Portal::Pristine::Hide(a_shape);
				hiddenShapes.emplace_back(a_shape);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	// ThirdPersonState::Update (ID 50896, vfunc 3 on SE/AE) computes `translation` and then
	// writes it to the camera root itself. Limiting afterwards keeps the engine's smoothing and
	// collision; only the horizontal distance is shortened (the height stays), and the camera
	// root is moved along.
	RE::NiPoint3                          carryOffset;
	float                                 carrySeconds = 0.0f;
	std::chrono::steady_clock::time_point carryStart;

	struct DoorPlane
	{
		RE::NiPoint3 center;
		RE::NiPoint3 inward;  // unit, horizontal: away from the player's space
		float        halfWidth;
	};
	std::optional<DoorPlane> plane;

	// How far the camera may get towards the doorway's plane.
	constexpr float kPlaneMargin = 8.0f;

	void Limit(RE::ThirdPersonState* a_state);

	// Horse and dragon cameras are third-person states with vtables of their own; hooked on
	// ThirdPersonState's alone, mounted the camera went through the doorway's plane unstopped.
	template <int Which>
	struct UpdateHook
	{
		static void Thunk(RE::ThirdPersonState* a_state, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
		{
			func(a_state, a_nextState);
			Limit(a_state);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The first-person camera at a doorway (SetFirstPersonStop). It follows the head, a little ahead
	// of the body and moving with each step; at the doorway it could get past the doorway's picture
	// (or within its own near distance of it) while the player stood in front: the picture vanished
	// for those frames and the room's own view behind it showed.
	struct FirstPersonStop
	{
		RE::NiPoint3 center;
		RE::NiPoint3 inward;
		float        halfWidth;
		float        maxDepth;
	};
	std::optional<FirstPersonStop> firstPersonStop;

	struct FirstPersonHook
	{
		static void Thunk(RE::FirstPersonState* a_state, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
		{
			func(a_state, a_nextState);
			auto* root = RE::PlayerCamera::GetSingleton()->cameraRoot.get();
			if (!firstPersonStop || !root) {
				return;
			}
			const auto& stop = *firstPersonStop;
			const auto  offset = root->world.translate - stop.center;
			const float depth = offset.x * stop.inward.x + offset.y * stop.inward.y;
			const float side = std::abs(offset.x * stop.inward.y - offset.y * stop.inward.x);
			if (depth > stop.maxDepth && depth < stop.maxDepth + 100.0f && side < stop.halfWidth + 60.0f) {
				root->local.translate -= stop.inward * (depth - stop.maxDepth);
				RE::NiUpdateData update;
				root->Update(update);
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

}

bool Portal::CameraLimit::InThirdPerson()
{
	// No state yet at the main menu, where the teleport hook already runs.
	const auto* state = RE::PlayerCamera::GetSingleton()->currentState.get();
	return state && (state->id == RE::CameraState::kThirdPerson || state->id == RE::CameraState::kMount || state->id == RE::CameraState::kDragon);
}

namespace
{
	using Portal::CameraLimit::InThirdPerson;

	void Limit(RE::ThirdPersonState* a_state)
	{
		if (carrySeconds > 0.0f) {
			// Right after a seamless crossing the engine restarts the camera at the player's head
			// (its collision test still sees the doorway as closed, and a teleport resets its
			// smoothing) and eases it back out. The camera keeps the place it had relative to
			// the player instead, and is handed back to the engine's own over the last part.
			const auto  now = std::chrono::steady_clock::now();
			const float left = carrySeconds - std::chrono::duration<float>(now - carryStart).count();
			if (left <= 0.0f) {
				carrySeconds = 0.0f;
			} else {
				const float weight = std::clamp(left / (carrySeconds * 0.6f), 0.0f, 1.0f);
				const auto  desired = RE::PlayerCharacter::GetSingleton()->GetPosition() + carryOffset;
				auto&       camera = a_state->translation;
				camera = camera + (desired - camera) * weight;
				if (auto* root = RE::PlayerCamera::GetSingleton()->cameraRoot.get()) {
					root->local.translate = camera;
					RE::NiUpdateData update;
					root->Update(update);
				}
			}
		}
		if (plane) {
			// A portal's doorway is only a picture of the other space: a camera that passes
			// through it stands in nothing. It stops in front of the plane like at a wall,
			// sliding towards the player.
			auto&       camera = a_state->translation;
			const auto  pos = RE::PlayerCharacter::GetSingleton()->GetPosition();
			const auto  DepthOf = [&](const RE::NiPoint3& a_point) { return (a_point.x - plane->center.x) * plane->inward.x + (a_point.y - plane->center.y) * plane->inward.y; };
			const float cameraDepth = DepthOf(camera);
			const float side = std::abs((camera.x - plane->center.x) * plane->inward.y - (camera.y - plane->center.y) * plane->inward.x);
			if (cameraDepth > -kPlaneMargin && cameraDepth < 300.0f && side < plane->halfWidth + 60.0f) {
				const float playerDepth = std::min(DepthOf(pos), -kPlaneMargin);
				const float t = cameraDepth - playerDepth > 0.001f ? (cameraDepth + kPlaneMargin) / (cameraDepth - playerDepth) : 1.0f;
				camera.x += (pos.x - camera.x) * t;
				camera.y += (pos.y - camera.y) * t;
				if (auto* root = RE::PlayerCamera::GetSingleton()->cameraRoot.get()) {
					root->local.translate = camera;
					RE::NiUpdateData update;
					root->Update(update);
				}
			}
		}
		if (!limit) {
			SetHidden(false);
			return;
		}
		auto*       player = RE::PlayerCharacter::GetSingleton();
		const auto  pos = player->GetPosition();
		auto&       camera = a_state->translation;
		const float dx = camera.x - pos.x;
		const float dy = camera.y - pos.y;
		const float distance = std::hypot(dx, dy);
		if (distance > *limit && distance > 0.001f) {
			const float scale = *limit / distance;
			camera.x = pos.x + dx * scale;
			camera.y = pos.y + dy * scale;
			if (auto* root = RE::PlayerCamera::GetSingleton()->cameraRoot.get()) {
				root->local.translate = camera;
				RE::NiUpdateData update;
				root->Update(update);
			}
		}
		SetHidden(std::min(distance, *limit) < kHidePlayerBelow);
	}
}

namespace Portal::CameraLimit
{
	void Install()
	{
		REL::Relocation<std::uintptr_t> thirdPerson{ RE::VTABLE_ThirdPersonState[0] };
		UpdateHook<0>::func = thirdPerson.write_vfunc(0x3, UpdateHook<0>::Thunk);
		REL::Relocation<std::uintptr_t> horse{ RE::VTABLE_HorseCameraState[0] };
		UpdateHook<1>::func = horse.write_vfunc(0x3, UpdateHook<1>::Thunk);
		REL::Relocation<std::uintptr_t> dragon{ RE::VTABLE_DragonCameraState[0] };
		UpdateHook<2>::func = dragon.write_vfunc(0x3, UpdateHook<2>::Thunk);
		REL::Relocation<std::uintptr_t> firstPerson{ RE::VTABLE_FirstPersonState[0] };
		FirstPersonHook::func = firstPerson.write_vfunc(0x3, FirstPersonHook::Thunk);
	}

	void Carry(const RE::NiPoint3& a_offsetFromPlayer, float a_seconds)
	{
		carryOffset = a_offsetFromPlayer;
		carrySeconds = a_seconds;
		carryStart = std::chrono::steady_clock::now();
	}

	void SetPlane(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth)
	{
		plane = DoorPlane{ a_center, a_inward, a_halfWidth };
	}

	void SetFirstPersonStop(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_maxDepth)
	{
		firstPersonStop = FirstPersonStop{ a_center, a_inward, a_halfWidth, a_maxDepth };
	}

	void ClearPlane()
	{
		plane.reset();
		firstPersonStop.reset();
	}

	bool HidePlayer()
	{
		return playerHidden;
	}

	void Set(std::optional<float> a_maxDistance)
	{
		limit = a_maxDistance;
		// The body is only shown again from a third-person camera's update; switched to first
		// person or a menu camera while hidden, it would otherwise stay hidden until the next one.
		if (!limit || !InThirdPerson()) {
			SetHidden(false);
		}
	}
}
