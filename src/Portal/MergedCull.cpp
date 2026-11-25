#include "Portal/MergedCull.h"

#include "Portal/NpcTravel.h"
#include "Portal/MergedView.h"
#include "Render/DoorwayPlanes.h"

namespace Portal::MergedCull
{
	namespace
	{
		using Render::Doorway::Plane;
		using Render::Doorway::Side;
		using Render::Doorway::SideOf;

		Mode                mode = Mode::kShapes;
		Mode                streetMode = Mode::kOff;
		std::vector<Room>   rooms;  // main thread, as is the view

		// Shown again after the view; only what was shown before, so the cull never undoes another
		// part's hiding (blockers in the doorway, shapes with stray bones).
		std::vector<RE::NiPointer<RE::NiAVObject>> hidden;
		bool                                       viewOpen = false;

		// The objects to leave alone, and every node above one (those can't be hidden whole).
		std::unordered_set<const RE::NiAVObject*> keep;
		std::unordered_set<const RE::NiAVObject*> aboveKept;

		Plane                  planes[5];
		std::span<const Plane> current = planes;  // what Classify tests: the doorway's, or a room's seen through portals

		// An interior built of rooms joined by portals (Understone Keep) is culled through them, as
		// the engine does when it is the current cell: the rooms seen through the doorway (or from
		// the room the camera stands in) and on through their portals are drawn, each tested
		// against what is seen of it; the other rooms not at all. Drawn whole within the doorway's
		// view, the keep's halls behind walls cost half the frame.
		bool roomCull = true;
		struct SeenRoom
		{
			const RE::BSMultiBoundRoom* room;
			std::vector<Plane>          planes;   // the door plane (seen through the doorway) and the edges of the last portal seen through
			bool                        several;  // reached more than one way: tested as the rest of the cell
		};
		std::vector<SeenRoom>                     seenRooms;   // this view, the room being culled
		std::vector<const RE::BSMultiBoundRoom*>  graphRooms;  // its cells' rooms; empty: no room is culled
		std::unordered_set<const RE::NiAVObject*> aboveRooms;  // never drawn whole without looking at the rooms below
		constexpr std::size_t                     kMaxRoomVisits = 256;
		std::size_t                               roomVisits = 0;
		std::string                               lastRoomsWhy;
		std::uint32_t                             lastRoomsSeen = 0;
		std::uint32_t                             lastRoomsTotal = 0;

		// A bound this big or this far from the door is no bound of the room as merged (one the
		// engine fixed in the cell's own coordinates and not yet moved, see RemoteCell::MoveBounds):
		// it says nothing, so the node is looked into and a shape left drawn.
		constexpr float kImplausible = 20000.0f;
		RE::NiPoint3    doorCenter;

		// Skinned shapes' bounds follow their bones only loosely; a person is tested as a whole,
		// generously, and never shape by shape.
		constexpr float kPersonRadius = 160.0f;
		constexpr float kPersonCentreHeight = 70.0f;

		struct Stats
		{
			std::uint64_t views = 0;
			std::uint64_t cameraInside = 0;
			std::uint64_t noneSeen = 0;  // everything hidden
			std::uint64_t shapeCulls = 0;
			std::uint32_t lastTested = 0;
			std::uint32_t lastHidden = 0;
			std::string   lastWhy;
			// Last 120 views: CPU time of the whole player view (cull included) and of the cull.
			std::array<float, 120> viewMs{};
			std::array<float, 120> cullMs{};
			std::size_t            index = 0;
		} stats;
		std::chrono::steady_clock::time_point viewStarted;
		float                                 lastCullMs = 0.0f;

		// The rooms of the merged cells (from their portal graphs; a few dozen at most), for
		// RoomCullHook. Culls run on job threads too.
		std::mutex                         roomsLock;
		std::vector<const RE::NiAVObject*> mergedRooms;  // under roomsLock; compared, never followed

		bool IsMergedRoom(const RE::NiAVObject* a_node)
		{
			std::scoped_lock lock(roomsLock);
			return std::ranges::find(mergedRooms, a_node) != mergedRooms.end();
		}

		// The merged cells' 3D. Written by Set on the main thread between views, read by the cull's
		// jobs during one, for every object culled: no lock.
		std::array<const RE::NiAVObject*, 64> mergedCellRoots{};
		std::size_t                           mergedCellRootCount = 0;

		bool IsMergedCellRoot(const RE::NiAVObject* a_node)
		{
			return a_node && std::ranges::find(mergedCellRoots.begin(), mergedCellRoots.begin() + mergedCellRootCount, a_node) != mergedCellRoots.begin() + mergedCellRootCount;
		}

		// A merged cell's root, one of its nodes (actors, statics...) or what hangs directly in one:
		// the main view's cull hands a cell's nodes and what they hold to its jobs (ID 36643), so the
		// cull doesn't always start at the cell's root.
		bool InMergedCell(const RE::NiAVObject* a_object)
		{
			return IsMergedCellRoot(a_object) || (a_object->parent && (IsMergedCellRoot(a_object->parent) || IsMergedCellRoot(a_object->parent->parent)));
		}

		// The occlusion planes and boxes of the player's space (the cliffs and walls around a door)
		// know nothing of doorways, and a merged room lies behind them. The per-object cull (ID 76553,
		// BSCullingProcess's Process) tests each object against the process's compound frustum,
		// which holds them (ID 76821): most of the Warrens seen from Markarth's mines wasn't drawn,
		// only its floor by the door and the sky beyond. A merged cell and the nodes in it (the main
		// view hands a cell's nodes to its jobs one by one, ID 36643) are culled ignoring multibounds,
		// which leaves the compound frustum out and tests against the view only; MergedCull itself
		// culls them against the doorway. Until the remote attach stopped changing the world scene's
		// graph, each one emptied the occluder list as a side effect (ENGINE.md, SetPortalGraph),
		// which hid this.
		struct ProcessHook
		{
			static void Thunk(RE::BSCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_alphaGroupIndex)
			{
				using Type = RE::BSCullingProcess::BSCPCullingType;
				if (a_process->cullMode.get() != Type::kNormal || !mergedCellRootCount || !InMergedCell(a_object)) {
					func(a_process, a_object, a_alphaGroupIndex);
					return;
				}
				a_process->cullMode = Type::kIgnoreMultiBounds;
				func(a_process, a_object, a_alphaGroupIndex);
				a_process->cullMode = Type::kNormal;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// An interior built of room bounds (Mistveil Keep) is culled through its portal graph, which
		// the world's scene doesn't have. Merged, the main view (ID 36643) missed two parts of it:
		// - A room's cull (ID 75886) walks into its children only when the culling process's graph
		//   lists the room, or when the process ignores multibounds. Nothing of the room was drawn
		//   but its particles (those come from the light scene, not the cull): the land behind the
		//   house showed through the doorway.
		// - What straddles a portal between two rooms (the great hall's walls) hangs under the
		//   portal's BSPortalSharedNode, in the first child of the cell's sixth node, which the main
		//   view leaves out: the graph culls it through the portals of the rooms seen.
		// For a merged room, kNormal processes ignore multibounds within the room, as the portal
		// pass does throughout (RenderPortal), and the room culls the shared geometry of its
		// portals, each portal from one of its two rooms only (so it is drawn once). Rooms are
		// always drawn (kAlwaysDraw), so every room of the cell gets here in every cull.
		struct RoomCullHook
		{
			static void Thunk(RE::BSMultiBoundRoom* a_room, RE::NiCullingProcess& a_process, std::int32_t a_alphaGroupIndex)
			{
				using Type = RE::BSCullingProcess::BSCPCullingType;
				// The engine's own function reads it as a BSCullingProcess, whichever process it is.
				auto& process = static_cast<RE::BSCullingProcess&>(a_process);
				// Also when ProcessHook has already switched the merged cell to ignoring multibounds:
				// passed on as it was, the room culled its children but not what straddles its
				// portals, and Mistveil Keep's hall stood without walls and ceiling.
				const auto mode = process.cullMode.get();
				if ((mode != Type::kNormal && mode != Type::kIgnoreMultiBounds) || !IsMergedRoom(a_room)) {
					func(a_room, a_process, a_alphaGroupIndex);
					return;
				}
				process.cullMode = Type::kIgnoreMultiBounds;
				func(a_room, a_process, a_alphaGroupIndex);
				for (auto* portal : a_room->portalList) {
					auto* shared = portal ? portal->portalSharedNode.get() : nullptr;
					if (!shared) {
						continue;
					}
					// A room left out by the cull through portals (Walk) has no OnVisible; the room on
					// the portal's other side draws what straddles it then.
					const auto drawn = [](const RE::BSMultiBoundRoom* a_side) { return a_side && IsMergedRoom(a_side) && !a_side->GetAppCulled(); };
					const auto* owner = drawn(portal->entranceRoom) ? portal->entranceRoom : portal->exitRoom;
					if (owner == a_room) {
						static REL::Relocation<void (*)(RE::NiAVObject*, RE::NiCullingProcess&, std::uint32_t)> cull{ REL::RelocationID(68916, 70267) };
						cull(shared, a_process, static_cast<std::uint32_t>(a_alphaGroupIndex));
					}
				}
				process.cullMode = mode;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		void Hide(RE::NiAVObject* a_object)
		{
			if (!a_object->GetAppCulled()) {
				a_object->SetAppCulled(true);
				hidden.emplace_back(a_object);
			}
		}

		void HideAll(RE::NiAVObject* a_object)
		{
			++stats.lastTested;
			if (keep.contains(a_object)) {
				return;
			}
			if (!aboveKept.contains(a_object)) {
				Hide(a_object);
				return;
			}
			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (child) {
						HideAll(child.get());
					}
				}
			}
		}

		// Wholly outside X (hide), wholly inside (draw as it is), or neither.
		Side Classify(const RE::NiPoint3& a_center, float a_radius)
		{
			if (a_radius <= 0.0f || a_radius > kImplausible || a_center.GetDistance(doorCenter) > kImplausible) {
				return Side::kCrosses;
			}
			bool within = true;
			for (const auto& plane : current) {
				const auto side = SideOf(plane, a_center, a_radius);
				if (side == Side::kOutside) {
					return Side::kOutside;
				}
				within &= side == Side::kInside;
			}
			return within ? Side::kInside : Side::kCrosses;
		}

		void WalkChildren(RE::NiAVObject* a_object);

		void Walk(RE::NiAVObject* a_object)
		{
			++stats.lastTested;
			if (keep.contains(a_object)) {
				return;
			}
			const bool guarded = aboveKept.contains(a_object);
			auto*      owner = a_object->GetUserData();
			if (auto* actor = owner ? owner->As<RE::Actor>() : nullptr; actor && actor->Get3D() == a_object) {
				const auto at = actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, kPersonCentreHeight };
				if (!guarded && Classify(at, std::max(kPersonRadius, a_object->worldBound.radius)) == Side::kOutside &&
					Classify(a_object->worldBound.center, a_object->worldBound.radius) == Side::kOutside) {
					Hide(a_object);
				}
				return;
			}
			if (auto* room = netimmerse_cast<RE::BSMultiBoundRoom*>(a_object); room && std::ranges::find(graphRooms, room) != graphRooms.end()) {
				const auto seen = std::ranges::find(seenRooms, room, &SeenRoom::room);
				if (seen == seenRooms.end()) {
					if (!guarded) {
						Hide(a_object);
						return;
					}
				} else if (!seen->several) {
					const auto outer = std::exchange(current, std::span<const Plane>(seen->planes));
					WalkChildren(a_object);
					current = outer;
					return;
				}
			}
			const auto side = Classify(a_object->worldBound.center, a_object->worldBound.radius);
			if (side == Side::kInside && !aboveRooms.contains(a_object)) {
				return;
			}
			// Not a room whole: it culls its portals' shared geometry (RoomCullHook), which may be
			// seen when the room's bound isn't. Its contents are looked at one by one instead.
			if (side == Side::kOutside && !guarded && !netimmerse_cast<RE::BSMultiBoundRoom*>(a_object)) {
				Hide(a_object);
				return;
			}
			WalkChildren(a_object);
		}

		void WalkChildren(RE::NiAVObject* a_object)
		{
			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (child) {
						Walk(child.get());
					}
				}
			}
		}

		// Some part of the doorway's rectangle may be in the camera's view: false only when all four
		// corners lie beyond one side of the view frustum. In camera space, not on screen: a corner
		// behind the camera has no meaningful screen position, and looking away from the door all
		// four are behind it.
		bool DoorwayInView(const Render::Doorway::Door& a_door)
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return true;
			}
			const auto& frustum = camera->GetRuntimeData2().viewFrustum;
			if (frustum.bOrtho) {
				return true;
			}
			// A camera looks along its local X, with Y up and Z to the right.
			const auto&        m = camera->world.rotate;
			const RE::NiPoint3 forward{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };
			const RE::NiPoint3 upward{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
			const RE::NiPoint3 rightward{ m.entry[0][2], m.entry[1][2], m.entry[2][2] };
			// Widened, so a doorway at the edge of the screen never pops.
			constexpr float kWiden = 1.15f;
			constexpr float kSlack = 30.0f;
			const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
			unsigned           outside = ~0u;  // bit per frustum side that every corner is beyond
			for (const float sx : { -1.0f, 1.0f }) {
				for (const float sy : { -1.0f, 1.0f }) {
					const auto  offset = a_door.center + a_door.right * (sx * a_door.halfWidth) + up * (sy * a_door.halfHeight) - camera->world.translate;
					const float f = offset.Dot(forward);
					const float u = offset.Dot(upward);
					const float r = offset.Dot(rightward);
					unsigned    beyond = 0;
					beyond |= f < -kSlack ? 1u : 0u;
					beyond |= r < frustum.fLeft * kWiden * f - kSlack ? 2u : 0u;
					beyond |= r > frustum.fRight * kWiden * f + kSlack ? 4u : 0u;
					beyond |= u < frustum.fBottom * kWiden * f - kSlack ? 8u : 0u;
					beyond |= u > frustum.fTop * kWiden * f + kSlack ? 16u : 0u;
					outside &= beyond;
				}
			}
			return outside == 0;
		}

		using Polygon = std::vector<RE::NiPoint3>;

		// What of a_polygon (convex) lies on the kept side of a_plane.
		Polygon Clip(const Polygon& a_polygon, const Plane& a_plane)
		{
			Polygon kept;
			for (std::size_t i = 0; i < a_polygon.size(); ++i) {
				const auto&  a = a_polygon[i];
				const auto&  b = a_polygon[(i + 1) % a_polygon.size()];
				const double atA = a_plane.At(a);
				const double atB = a_plane.At(b);
				if (atA >= 0.0) {
					kept.push_back(a);
				}
				if ((atA >= 0.0) != (atB >= 0.0)) {
					kept.push_back(a + (b - a) * static_cast<float>(atA / (atA - atB)));
				}
			}
			return kept;
		}

		// A portal's rectangle where the room is drawn. BSOcclusionPlane::corners is all zeros in
		// a loaded cell; the rectangle is its local XZ plane, size its half width and half height
		// (Understone Keep's portals across corridors: rotation identity or a quarter turn about Z),
		// in the cell's own coordinates, which RemoteCell::Merge moves to the door with the cell's 3D.
		Polygon PortalPolygon(const RE::BSPortal& a_portal, const RE::NiTransform& a_toWorld)
		{
			Polygon polygon;
			for (const auto& [x, z] : { std::pair{ -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } }) {
				const RE::NiPoint3 local{ x * a_portal.size.x, 0.0f, z * a_portal.size.y };
				polygon.push_back(a_toWorld * (a_portal.translation + a_portal.rotation * local));
			}
			return polygon;
		}

		// a_base: the door plane, seen through the doorway; none from inside the room.
		void SeeRoom(const RE::BSMultiBoundRoom* a_room, const std::vector<Plane>& a_planes, const std::optional<Plane>& a_base, const RE::NiPoint3& a_eye,
			const RE::NiTransform& a_toWorld, std::vector<const RE::BSPortal*>& a_path)
		{
			if (++roomVisits > kMaxRoomVisits) {
				return;
			}
			if (const auto seen = std::ranges::find(seenRooms, a_room, &SeenRoom::room); seen != seenRooms.end()) {
				seen->several = true;
			} else {
				seenRooms.push_back({ a_room, a_planes, false });
			}
			for (const auto* portal : a_room->portalList) {
				const auto* next = portal->entranceRoom == a_room ? portal->exitRoom : portal->entranceRoom;
				if (!next || std::ranges::find(graphRooms, next) == graphRooms.end() || std::ranges::find(a_path, portal) != a_path.end()) {
					continue;
				}
				auto polygon = PortalPolygon(*portal, a_toWorld);
				for (const auto& plane : a_planes) {
					polygon = Clip(polygon, plane);
					if (polygon.size() < 3) {
						break;
					}
				}
				if (polygon.size() < 3) {
					continue;
				}
				// Seen through the part of the portal that is seen: the base plane, and a plane through
				// the camera and each edge of that part. No plane of the portal itself: a room's
				// shapes reach past its portals.
				RE::NiPoint3 middle;
				for (const auto& corner : polygon) {
					middle += corner;
				}
				middle = middle / static_cast<float>(polygon.size());
				std::vector<Plane> through;
				if (a_base) {
					through.push_back(*a_base);
				}
				for (std::size_t i = 0; i < polygon.size(); ++i) {
					const auto& a = polygon[i];
					const auto& b = polygon[(i + 1) % polygon.size()];
					// An edge in line with the camera gives no plane; leaving it out sees more.
					if ((a - a_eye).Cross(b - a).Length() > 1.0f) {
						through.push_back(Render::Doorway::EdgePlane(a_eye, a, b - a, middle));
					}
				}
				a_path.push_back(portal);
				SeeRoom(next, through, a_base, a_eye, a_toWorld, a_path);
				a_path.pop_back();
			}
		}

		// The camera's view frustum, widened a little as in DoorwayInView.
		std::optional<std::vector<Plane>> ViewPlanes(const RE::NiCamera& a_camera)
		{
			const auto& frustum = a_camera.GetRuntimeData2().viewFrustum;
			if (frustum.bOrtho) {
				return std::nullopt;
			}
			// A camera looks along its local X, with Y up and Z to the right.
			const auto&        m = a_camera.world.rotate;
			const RE::NiPoint3 forward{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };
			const RE::NiPoint3 upward{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
			const RE::NiPoint3 rightward{ m.entry[0][2], m.entry[1][2], m.entry[2][2] };
			constexpr float    kWiden = 1.15f;
			const auto&        eye = a_camera.world.translate;
			const auto         corner = [&](float a_right, float a_up) { return eye + forward + rightward * (a_right * kWiden) + upward * (a_up * kWiden); };
			const RE::NiPoint3 corners[] = { corner(frustum.fLeft, frustum.fBottom), corner(frustum.fRight, frustum.fBottom), corner(frustum.fRight, frustum.fTop),
				corner(frustum.fLeft, frustum.fTop) };
			std::vector<Plane> view;
			for (std::size_t i = 0; i < std::size(corners); ++i) {
				const auto& a = corners[i];
				const auto& b = corners[(i + 1) % std::size(corners)];
				view.push_back(Render::Doorway::EdgePlane(eye, a, b - a, eye + forward));
			}
			return view;
		}

		// The rooms of a_cell seen from the camera, through the doorway or (a_inside) from the room
		// it stands in: graphRooms, seenRooms. False when its rooms aren't culled (no room graph, or
		// no room to start from).
		bool SeeRooms(RE::TESObjectCELL* a_cell, const Render::Doorway::Door& a_door, const RE::NiCamera& a_camera, bool a_inside)
		{
			const auto& eye = a_camera.world.translate;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			auto* graph = loaded ? loaded->portalGraph.get() : nullptr;
			auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
			if (!graph || !cell3D || graph->rooms.empty()) {
				return false;
			}
			for (const auto& room : graph->rooms) {
				graphRooms.push_back(room.get());
			}
			// The rooms the camera stands in, or those the doorway opens into: a little way into the
			// merged space.
			std::vector<const RE::BSMultiBoundRoom*> entrances;
			const auto enter = [&](const RE::NiPoint3& a_probe) {
				for (const auto& room : graph->rooms) {
					if (room->QPointWithin(a_probe) && std::ranges::find(entrances, room.get()) == entrances.end()) {
						entrances.push_back(room.get());
					}
				}
			};
			if (a_inside) {
				enter(eye);
			} else {
				for (const float depth : { 16.0f, 64.0f, 160.0f }) {
					enter(a_door.center + a_door.normal * depth);
				}
			}
			const auto start = a_inside ? ViewPlanes(a_camera) : std::optional(std::vector<Plane>(std::begin(planes), std::end(planes)));
			if (entrances.empty() || !start) {
				lastRoomsWhy = a_inside ? "the camera in none of the rooms" : "the door in none of the rooms";
				graphRooms.clear();
				return false;
			}
			const auto                       base = a_inside ? std::nullopt : std::optional<Plane>(planes[0]);
			std::vector<const RE::BSPortal*> path;
			for (const auto* entrance : entrances) {
				SeeRoom(entrance, *start, base, eye, cell3D->world, path);
			}
			if (roomVisits > kMaxRoomVisits) {
				lastRoomsWhy = "too many ways through the portals";
				graphRooms.clear();
				seenRooms.clear();
				return false;
			}
			for (const auto* room : graphRooms) {
				for (auto* up = room->parent; up; up = up->parent) {
					aboveRooms.insert(up);
				}
			}
			lastRoomsWhy = std::format("{} {} room(s), {} way(s) through portals", entrances.size(), a_inside ? "camera" : "entrance", roomVisits);
			lastRoomsSeen += static_cast<std::uint32_t>(seenRooms.size());
			lastRoomsTotal += static_cast<std::uint32_t>(graph->rooms.size());
			return true;
		}

		void ForgetRooms()
		{
			graphRooms.clear();
			seenRooms.clear();
			aboveRooms.clear();
			roomVisits = 0;
			current = planes;
		}

		bool HasRooms(const Room& a_room)
		{
			return std::ranges::any_of(a_room.cells, [](RE::TESObjectCELL* a_cell) {
				auto* loaded = a_cell->GetRuntimeData().loadedData;
				return loaded && loaded->portalGraph && !loaded->portalGraph->rooms.empty();
			});
		}

		// The rooms are culled one after the other, each against its own doorway; they are disjoint
		// subtrees, so what one keeps says nothing about another.
		// a_unseen: no line of sight reaches the room (the camera stands in another one).
		void CullRoom(const Room& a_room, bool a_unseen)
		{
			const auto* room = &a_room;
			const auto& door = room->door;
			// The camera stands in the room: it is the space seen everywhere, but for its rooms the
			// camera can't see into.
			const bool inside = door.cameraMerged && mode != Mode::kHideAll;
			if (inside) {
				++stats.cameraInside;
				stats.lastWhy = "camera in the room";
				if (!roomCull || !HasRooms(a_room)) {
					return;
				}
			}
			// The street casts the sun's shadows (Interior Sun), into the room too: hidden whole while
			// the doorway was off screen, the sunlit patches on the room's walls changed; shape by
			// shape, a porch beam's shadow seen through the doorway thinned. Off unless asked for.
			const auto wanted = room->street && mode != Mode::kHideAll ? std::min(mode, streetMode) : mode;
			if (wanted == Mode::kOff) {
				stats.lastWhy = "street: not culled";
				return;
			}
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return;
			}
			doorCenter = door.center;
			const char* none = inside                                               ? nullptr :
			                   mode == Mode::kHideAll                               ? "hidden by hand" :
			                   a_unseen                                             ? "camera in another room" :
			                   room->doorShut                                      ? "door shut" :
			                   !Render::Doorway::Planes(door, camera->world.translate, planes) ? "camera behind the door plane" :
			                   !DoorwayInView(door)                                 ? "doorway off screen" :
			                                                                          nullptr;
			if (!none && wanted == Mode::kWhole) {
				stats.lastWhy = inside ? "camera in the room" : "doorway seen";
				return;
			}
			keep.clear();
			aboveKept.clear();
			std::vector<RE::NiAVObject*> withWorld, withRoom;
			Portal::NpcTravel::MergedOverrides(withWorld, withRoom);
			const auto keepObject = [](const RE::NiAVObject* a_object) {
				keep.insert(a_object);
				for (auto* up = a_object->parent; up; up = up->parent) {
					aboveKept.insert(up);
				}
			};
			for (const auto& object : room->keep) {
				keepObject(object.get());
			}
			// People walking in from the street, drawn with it until they pass the door plane.
			for (auto* object : withWorld) {
				keepObject(object);
			}
			auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			const auto merged = Portal::MergedView::MergedCells();
			for (auto* cell : room->cells) {
				// Only cells still merged: a room the player has just walked into is theirs now.
				if (cell == here || std::ranges::find(merged, cell) == merged.end()) {
					continue;
				}
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* root = loaded ? loaded->cell3D.get() : nullptr) {
					if (none) {
						HideAll(root);
					} else {
						if (roomCull) {
							SeeRooms(cell, door, *camera, inside);
						}
						// From inside, only rooms are culled: the engine culls the rest against the view.
						if (inside) {
							current = {};
						}
						Walk(root);
						ForgetRooms();
					}
				}
			}
			for (const auto& root : inside ? std::span<const RE::NiPointer<RE::NiAVObject>>() : std::span(room->roots)) {
				if (none) {
					HideAll(root.get());
				} else {
					Walk(root.get());
				}
			}
			stats.lastWhy = none ? none : inside ? "camera in the room" : "doorway seen";
			++(none ? stats.noneSeen : stats.shapeCulls);
		}

		void Cull()
		{
			stats.lastTested = 0;
			lastRoomsSeen = lastRoomsTotal = 0;
			lastRoomsWhy = roomCull ? "no room graph" : "off";
			if (rooms.empty() || mode == Mode::kOff) {
				stats.lastWhy = rooms.empty() ? "nothing merged" : "off";
				return;
			}
			++stats.views;
			std::string why;
			const bool  cameraInRoom = std::ranges::any_of(rooms, [](const Room& a_room) { return a_room.door.cameraMerged; });
			for (const auto& room : rooms) {
				CullRoom(room, cameraInRoom && !room.door.cameraMerged);
				why += (why.empty() ? "" : "; ") + stats.lastWhy;
			}
			stats.lastWhy = std::move(why);
		}
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ REL::RelocationID(287733, 238170) };
		const auto                      expected = REL::Relocation<std::uintptr_t>{ REL::RelocationID(74147, 75886) }.address();
		const auto                      slot = *reinterpret_cast<const std::uintptr_t*>(vtable.address() + 0x34 * sizeof(std::uintptr_t));
		if (slot != expected) {
			logger::warn("portal: BSMultiBoundRoom's OnVisible is not ID 75886; merged rooms built of room bounds stay unseen");
			return;
		}
		RoomCullHook::func = vtable.write_vfunc(0x34, RoomCullHook::Thunk);
		// BSCullingProcess's and BSGeometryListCullingProcess's (the main view's, ID 415022) share it.
		const auto processExpected = REL::Relocation<std::uintptr_t>{ REL::RelocationID(74804, 76553) }.address();
		for (const auto vtableId : { REL::RelocationID(288074, 238422), REL::RelocationID(242858, 196460) }) {
			REL::Relocation<std::uintptr_t> processVtable{ vtableId };
			if (*reinterpret_cast<const std::uintptr_t*>(processVtable.address() + 0x16 * sizeof(std::uintptr_t)) != processExpected) {
				logger::warn("portal: a culling process's Process (vtable ID {}) is not ID 76553; the player's space's occluders hide merged rooms", vtableId.id());
				continue;
			}
			ProcessHook::func = processVtable.write_vfunc(0x16, ProcessHook::Thunk);
		}
	}

	void Set(std::vector<Room> a_rooms)
	{
		rooms = std::move(a_rooms);
		// Not the room the player has walked into: the engine culls it with its own graph then.
		auto*                              here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		std::vector<const RE::NiAVObject*> found;
		std::vector<const RE::NiAVObject*> roots;
		for (const auto& room : rooms) {
			for (auto* cell : room.cells) {
				auto* loaded = cell != here ? cell->GetRuntimeData().loadedData : nullptr;
				// Not a street merged into an interior: hung in the room at its door, it is culled through
				// that room's portals, which the compound frustum holds (Riften's jail).
				if (auto* root = loaded && !room.street ? loaded->cell3D.get() : nullptr) {
					roots.push_back(root);
				}
				if (auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
					for (const auto& node : graph->rooms) {
						found.push_back(node.get());
					}
				}
			}
		}
		mergedCellRootCount = std::min(roots.size(), mergedCellRoots.size());
		std::copy_n(roots.begin(), mergedCellRootCount, mergedCellRoots.begin());
		std::scoped_lock lock(roomsLock);
		mergedRooms = std::move(found);
	}

	void BeforeView()
	{
		viewStarted = std::chrono::steady_clock::now();
		if (viewOpen) {
			AfterView();  // not closed last time; the hidden must not stay hidden
		}
		viewOpen = true;
		Cull();
		stats.lastHidden = static_cast<std::uint32_t>(hidden.size());
		lastCullMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - viewStarted).count();
	}

	void AfterView()
	{
		for (auto& object : hidden) {
			object->SetAppCulled(false);
		}
		hidden.clear();
		if (std::exchange(viewOpen, false)) {
			const auto slot = stats.index++ % stats.viewMs.size();
			stats.viewMs[slot] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - viewStarted).count();
			stats.cullMs[slot] = lastCullMs;
		}
	}

	void SetRoomCull(bool a_on)
	{
		roomCull = a_on;
		stats.index = 0;
	}

	void SetMode(Mode a_mode)
	{
		mode = a_mode;
		// The view times are compared per mode.
		stats.index = 0;
	}

	void SetStreetMode(Mode a_mode)
	{
		streetMode = a_mode;
		stats.index = 0;
	}

	json Describe()
	{
		const auto count = std::min(stats.index, stats.viewMs.size());
		const auto summary = [&](const std::array<float, 120>& a_ms) -> json {
			if (!count) {
				return nullptr;
			}
			std::vector<float> sorted(a_ms.begin(), a_ms.begin() + count);
			std::ranges::sort(sorted);
			return { { "avg", std::accumulate(sorted.begin(), sorted.end(), 0.0f) / count }, { "median", sorted[count / 2] }, { "p95", sorted[count * 95 / 100] },
				{ "max", sorted.back() } };
		};
		return {
			{ "mode", static_cast<int>(mode) },
			{ "streetMode", static_cast<int>(streetMode) },
			{ "merged", rooms.size() },
			{ "views", stats.views },
			{ "cameraInside", stats.cameraInside },
			{ "noneSeen", stats.noneSeen },
			{ "shapeCulls", stats.shapeCulls },
			{ "lastWhy", stats.lastWhy },
			{ "lastTested", stats.lastTested },
			{ "lastHidden", stats.lastHidden },
			{ "viewMs", summary(stats.viewMs) },
			{ "cullMs", summary(stats.cullMs) },
			{ "rooms", { { "on", roomCull }, { "seen", lastRoomsSeen }, { "of", lastRoomsTotal }, { "why", lastRoomsWhy } } },
		};
	}
}
