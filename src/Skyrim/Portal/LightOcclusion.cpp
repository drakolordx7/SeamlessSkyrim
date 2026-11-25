#include "Skyrim/Portal/LightOcclusion.h"

#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Render/Wire.h"

namespace
{
	// With the Havok world: one cache serves every space, and a space's pairs outlive its crossings.
	struct Key
	{
		const RE::bhkWorld*   world;
		const RE::BSGeometry* geometry;
		const RE::NiLight*    light;
		bool operator==(const Key&) const = default;
	};

	struct KeyHash
	{
		std::size_t operator()(const Key& a_key) const noexcept
		{
			// The table takes the low bits, and pointers' low bits are zeros: mixed (splitmix64's finaliser).
			auto mix = [](std::uint64_t a_x) {
				a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
				a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
				return a_x ^ (a_x >> 31);
			};
			const auto bits = [](const void* a_pointer) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer)); };
			return static_cast<std::size_t>(mix(bits(a_key.geometry) ^ mix(bits(a_key.light) ^ mix(bits(a_key.world)))));
		}
	};

	// All in the Havok world's coordinates, which don't change when the space is drawn elsewhere
	// (merged at its door, unmerged, walked into and out of): measured once, a pair holds until the
	// object, the light or the door it was measured with moves. Kept in where it is drawn, the
	// cache was dropped with every crossing and the whole room measured again: Understone Keep,
	// ~12000 pairs, took 40% of the main thread for a second or two after walking out of it.
	struct Entry
	{
		float        visibility;
		RE::NiPoint3 center;  // the object's (actors move)
		RE::NiPoint3 light;   // a light carried about, or a new light at a freed one's address
		RE::NiPoint3 door;    // ThroughClosedDoor's door (another door of the same space)
	};
	struct LightEntry
	{
		const RE::NiLight* light;
		Entry              entry;
	};
	// By shape (Key's light null), its lights side by side: the renderer asks for each of a shape's
	// shadow-casting lights in a row, every draw. Keyed by pair, each question was a lookup in a
	// table of thousands of nodes, a cache miss each (~0.25 ms a frame, three rooms merged in Whiterun's market).
	std::unordered_map<Key, std::vector<LightEntry>, KeyHash> cache;
	std::size_t                                             cachedPairs = 0;
	// Spaces come and go; their pairs are dropped only when there are this many.
	constexpr std::size_t kMaxCached = 200000;

	// See OutsideShell; dropped with cache. With where the light was: a light carried about (a
	// torch) or a new light at a freed one's address is judged again.
	struct ShellVerdict
	{
		bool         outside = false;
		RE::NiPoint3 at;
		bool         interior = false;
	};
	// One per room merged at once (Render::Backend::MergedRooms), by the room's slot: each room's
	// lights are measured in its own Havok world, placed by its own door. Slot 0 also serves the
	// portal pass.
	struct Space
	{
		RE::bhkWorld*                                        world = nullptr;
		RE::NiTransform                                      toPhysics;  // see SetWorld
		std::optional<Portal::LightOcclusion::RoomDoor>      door;       // see SetWorld
		bool                                                 interior = false;  // see SetWorld
	};
	std::array<Space, ::Render::kMaxRooms> spaces;
	// With the world, like the cache.
	std::unordered_map<Key, ShellVerdict, KeyHash> outsideShell;  // geometry null
	std::uint32_t                              unmeasuredThisFrame = 0;
	std::uint64_t                              raysTotal = 0;
	std::uint32_t                              frame = 0;  // advanced with slot 0's SetWorld, once per frame
	// Measuring a whole room at once (thousands of pairs) would be one long frame; the rest is
	// measured over the next frames, and until then a pair counts as half lit. In time, not rays: a
	// ray in a Dwemer hall's mesh costs ~3 us, and 4000 rays a frame were 12 ms.
	constexpr double                           kBudgetMsPerFrame = 1.5;
	double                                     msThisFrame = 0.0;
	constexpr float         kUnmeasured = 0.5f;
	// The door panel's half thickness and a little: a point on the panel is still in the room.
	constexpr float kDoorSlack = 8.0f;
	// Around the opening: the frame, which stands where the closed door's shadow would fall.
	constexpr float kDoorMargin = 64.0f;

	// In the room's own world its door is closed, and the room's light stops there. Its Havok
	// world has the door open (the portal holds it open, for walking through), so rays pass:
	// Lux's strong lights outside the entrance lit the floor inside it, and the frame's outer step
	// (which sticks out beyond the door, where nothing of the room shields it) glowed from the
	// street.
	bool ThroughClosedDoor(const Space& a_space, const RE::NiPoint3& a_light, const RE::NiPoint3& a_point)
	{
		const auto& door = a_space.door;
		if (!door) {
			return false;
		}
		const auto right = RE::NiPoint3{ 0.0f, 0.0f, 1.0f }.Cross(door->intoRoom);
		const auto inOpening = [&](const RE::NiPoint3& a_at) {
			const auto offset = a_at - door->center;
			return std::abs(offset.Dot(right)) < door->halfWidth + kDoorMargin && std::abs(offset.z) < door->halfHeight + kDoorMargin;
		};
		const float lightSide = (a_light - door->center).Dot(door->intoRoom);
		const float pointSide = (a_point - door->center).Dot(door->intoRoom);
		// Just outside the door, in front of the opening (the frame's outer step): the closed
		// door's shadow from the room, or the street the room doesn't light. Only just: unbounded,
		// a whole column behind the entrance's plane went dark (a room reaching round its entry).
		if (pointSide < -kDoorSlack && pointSide > -(kDoorSlack + kDoorMargin) && inOpening(a_point)) {
			return true;
		}
		// Otherwise only a line crossing the door's plane passes the closed door.
		if ((lightSide >= -kDoorSlack) == (pointSide >= -kDoorSlack)) {
			return false;
		}
		return inOpening(a_light + (a_point - a_light) * (lightSide / (lightSide - pointSide)));
	}

	// Whether the straight line between the two points is free of static collision (line-of-sight
	// layer, like the engine's own LOS checks). A hit in the last few units counts as free: that
	// is the surface the point lies on.
	bool Clear(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick;
		pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
		++raysTotal;
		if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
			return true;
		}
		const float length = a_from.GetDistance(a_to);
		return (1.0f - pick.rayOutput.hitFraction) * length < 12.0f;
	}

	// How far a ray from a light goes before it counts as gone into the void around the cell. Four
	// cells: at one, the far walls of a big hall (a Dwemer one, a cavern) counted as void, and
	// lights in the middle of it were left out.
	constexpr float kShellReach = 16384.0f;

	// Whether a light stands outside the room's shell: above its roof or behind a window, where
	// Lux puts strong lights whose shadows draw sunbeams through the openings. An interior has no
	// world around it, so most rays from such a light leave into the void; from a light in the
	// room they meet a wall, the floor or the ceiling (a ceiling without collision lets one out).
	// Measured per object, such a light lit whole floor pieces that a grate's opening saw:
	// Whiterun's Hall of the Dead, seen from the street, had squares of sunlight on its floor.
	bool OutsideShell(RE::bhkWorld* a_world, const RE::NiPoint3& a_source)
	{
		const RE::NiPoint3 directions[] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		const float        scale = RE::bhkWorld::GetWorldScale();
		int                escaped = 0;
		for (const auto& direction : directions) {
			const auto      to = a_source + direction * kShellReach;
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_source.x * scale, a_source.y * scale, a_source.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
			++raysTotal;
			if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
				++escaped;
			}
		}
		return escaped >= 3;
	}

	// Its beams need the shadow map this view doesn't have: left out, rather than lighting whole
	// shapes that only a sliver of the beam falls on. Six rays once per light, so judged outside
	// the ray budget: within it, the shapes not measured yet were half lit by it meanwhile, and a
	// view of the whole room at once (the doorway picture's camera) had Whiterun's Hall of the
	// Dead and Jorrvaskr's Warmaidens far too bright.
	bool LightOutside(const Space& a_space, const RE::NiLight* a_light)
	{
		const auto source = a_space.toPhysics * a_light->world.translate;
		auto [verdict, added] = outsideShell.try_emplace(Key{ a_space.world, nullptr, a_light });
		if (added || verdict->second.at.GetSquaredDistance(source) > 25.0f * 25.0f || verdict->second.interior != a_space.interior) {
			verdict->second = { a_space.interior && OutsideShell(a_space.world, source), source, a_space.interior };
		}
		return verdict->second.outside;
	}

	float Measure(const Space& a_space, const RE::BSGeometry* a_geometry, const RE::NiLight* a_light)
	{
		const auto& bound = a_geometry->worldBound;
		const auto  source = a_space.toPhysics * a_light->world.translate;
		// The bound's centre and six points around it: big shapes (a floor, a wall) are partly lit.
		const float  r = bound.radius * 0.6f;
		const RE::NiPoint3 offsets[] = { {}, { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r * 0.5f }, { 0, 0, -r * 0.5f } };
		int          seen = 0;
		for (const auto& offset : offsets) {
			const auto point = a_space.toPhysics * (bound.center + offset);
			seen += !ThroughClosedDoor(a_space, source, point) && Clear(a_space.world, source, point) ? 1 : 0;
		}
		return static_cast<float>(seen) / std::size(offsets);
	}
}

namespace Portal::LightOcclusion
{
	void SetWorld(int a_slot, RE::bhkWorld* a_world, const RE::NiTransform& a_toPhysics, const std::optional<RoomDoor>& a_door, bool a_interior)
	{
		spaces[a_slot] = { a_world, a_toPhysics, a_door, a_interior };
		if (a_slot == 0) {
			// Once per frame: slot 0 is set every frame, before the others.
			++frame;
			unmeasuredThisFrame = 0;
			msThisFrame = 0.0;
			if (cachedPairs > kMaxCached) {
				cache.clear();
				cachedPairs = 0;
				outsideShell.clear();
			}
		}
	}

	std::uint32_t UnmeasuredThisFrame()
	{
		return unmeasuredThisFrame;
	}

	float Visibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light)
	{
		// A shape of a room merged beside the shown one is lit by that room's lights alone. Asked
		// for each of a shape's shadow-casting lights in a row: the room, the shape's place and its
		// lights looked up once per shape. The lights' list stays valid until the cache is cleared,
		// at the start of a frame (SetWorld).
		static thread_local const RE::BSGeometry*   lastShape = nullptr;
		static thread_local std::uint32_t           lastFrame = 0;
		static thread_local Space*                  lastSpace = nullptr;
		static thread_local RE::NiPoint3            lastCenter;
		static thread_local std::vector<LightEntry>* lastLights = nullptr;
		if (a_geometry != lastShape || frame != lastFrame) {
			lastShape = a_geometry;
			lastFrame = frame;
			const int slot = a_geometry ? Portal::MergedView::RoomOf(a_geometry) : -1;
			lastSpace = &spaces[slot > 0 && slot < static_cast<int>(spaces.size()) ? slot : 0];
			lastLights = nullptr;
			if (lastSpace->world && a_geometry) {
				lastCenter = lastSpace->toPhysics * a_geometry->worldBound.center;
				lastLights = &cache[Key{ lastSpace->world, a_geometry, nullptr }];
			}
		}
		auto& space = *lastSpace;
		if (!lastLights || !a_light) {
			return 1.0f;
		}
		const auto&     center = lastCenter;
		const auto      light = space.toPhysics * a_light->world.translate;
		const auto      door = space.door ? space.door->center : RE::NiPoint3{};
		constexpr float kMoved = 25.0f * 25.0f;
		auto            cached = std::ranges::find(*lastLights, a_light, &LightEntry::light);
		if (cached != lastLights->end() && cached->entry.center.GetSquaredDistance(center) < kMoved && cached->entry.light.GetSquaredDistance(light) < kMoved &&
			cached->entry.door.GetSquaredDistance(door) < kMoved) {
			return cached->entry.visibility;
		}
		RE::BSReadLockGuard lock(space.world->worldLock);
		if (LightOutside(space, a_light)) {
			return 0.0f;
		}
		if (msThisFrame >= kBudgetMsPerFrame) {
			++unmeasuredThisFrame;
			return kUnmeasured;
		}
		const auto  started = std::chrono::steady_clock::now();
		const float visibility = Measure(space, a_geometry, a_light);
		msThisFrame += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		const Entry entry{ visibility, center, light, door };
		if (cached != lastLights->end()) {
			cached->entry = entry;
		} else {
			lastLights->push_back({ a_light, entry });
			++cachedPairs;
		}
		return visibility;
	}

	json Describe()
	{
		json bySlot = json::array();
		for (const auto& space : spaces) {
			bySlot.push_back({ { "world", space.world != nullptr } });
		}
		return { { "pairs", cachedPairs }, { "raysTotal", raysTotal }, { "unmeasuredThisFrame", unmeasuredThisFrame }, { "world", spaces[0].world != nullptr },
			{ "slots", bySlot } };
	}
}
