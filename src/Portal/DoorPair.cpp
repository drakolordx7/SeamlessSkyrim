#include "Portal/DoorPair.h"

#include "Paths.h"
#include "Portal/Pristine.h"
#include "Portal/PortalManager.h"

namespace Portal::Doors
{
	RE::TESObjectREFR* Door(RE::FormID a_id)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
		if (!door || !door->Get3D()) {
			throw std::runtime_error(std::format("door {:08X} has no 3D", a_id));
		}
		return door;
	}

	// ---- the door's own geometry -------------------------------------------------------------
	// The base object's bounds (OBND) are no measure of the panel: a load door's mesh also holds a
	// box behind the panel that hides the void behind a closed door ("DoorToBlack"), handles,
	// sometimes a frame or a piece of wall. The shack door of Whiterun's guard house is 51 units
	// deep for an 8 unit panel; centred on those bounds, every doorway was mapped 20-30 units off
	// (both doors of a pair carry the box on opposite sides) and the portal stood that far in
	// front of the door. The panel is measured from the mesh instead: the leaves are the nodes the
	// door's own opening animation turns, and its faces are where most of its surface faces the
	// doorway (a handle or the box's side walls hardly do).
	namespace
	{
		// Every caller is on the main thread today (player update, render hooks, the menu, bridge
		// commands run as main-thread tasks); the swung leaves are read while measuring.
		// Recursive: Swing measures and reads under the same lock.
		std::recursive_mutex geometryLock;

		// Doors swung open (Swing), with the transforms their leaves had closed.
		struct SwungLeaf
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiTransform               closedLocal;
		};
		std::unordered_map<RE::FormID, std::vector<SwungLeaf>> swungClosed;
		// The shapes of a swung door's leaves drawn from behind too (Leaf::oneSided), to be put back.
		std::unordered_map<RE::FormID, std::vector<RE::NiPointer<RE::BSShaderProperty>>> swungTwoSided;

		const RE::NiTransform* ClosedLocal(RE::FormID a_door, const RE::NiAVObject* a_node)
		{
			const auto it = swungClosed.find(a_door);
			if (it != swungClosed.end()) {
				for (const auto& leaf : it->second) {
					if (leaf.node.get() == a_node) {
						return &leaf.closedLocal;
					}
				}
			}
			return nullptr;
		}

		// a_object's transform in the door's model space (its 3D root's frame), with the door closed.
		RE::NiTransform ToRoot(RE::FormID a_door, const RE::NiAVObject* a_object, const RE::NiAVObject* a_root)
		{
			RE::NiTransform result;
			for (auto* node = a_object; node && node != a_root; node = node->parent) {
				const auto* closed = ClosedLocal(a_door, node);
				result = (closed ? *closed : node->local) * result;
			}
			return result;
		}

		// The nodes the door's opening animation moves (NiMultiTargetTransformController targets),
		// outermost only: a handle animated on its panel moves with it.
		std::vector<RE::NiAVObject*> AnimatedNodes(RE::NiAVObject* a_root)
		{
			std::vector<RE::NiAVObject*> nodes;
			for (auto* controller = a_root->GetControllers(); controller; controller = controller->next.get()) {
				auto* multi = netimmerse_cast<RE::NiMultiTargetTransformController*>(controller);
				for (std::uint16_t i = 0; multi && multi->targets && i < multi->numInterps; ++i) {
					auto* target = multi->targets[i];
					if (target && target != a_root && std::ranges::find(nodes, target) == nodes.end()) {
						nodes.push_back(target);
					}
				}
			}
			std::erase_if(nodes, [&](RE::NiAVObject* a_node) {
				for (auto* parent = a_node->parent; parent && parent != a_root; parent = parent->parent) {
					if (std::ranges::find(nodes, parent) != nodes.end()) {
						return true;
					}
				}
				return false;
			});
			return nodes;
		}

		// a_visit(a, b, c) for each triangle of the shape, put through a_toRoot. False when the
		// shape keeps no copy of its mesh in memory.
		template <class F>
		bool ForEachTriangle(RE::BSGeometry* a_shape, const RE::NiTransform& a_toRoot, F&& a_visit)
		{
			auto* triShape = netimmerse_cast<RE::BSTriShape*>(a_shape);
			auto& data = a_shape->GetGeometryRuntimeData();
			auto* mesh = data.rendererData;
			if (!triShape || !mesh || !mesh->rawVertexData || !mesh->rawIndexData) {
				return false;
			}
			const auto          desc = *reinterpret_cast<const std::uint64_t*>(&data.vertexDesc);
			const std::uint32_t stride = static_cast<std::uint32_t>(desc & 0xF) * 4;  // positions first, as floats
			const auto&         counts = triShape->GetTrishapeRuntimeData();
			if (stride < 12) {
				return false;
			}
			const auto vertex = [&](std::uint16_t a_index) {
				const auto* p = reinterpret_cast<const float*>(mesh->rawVertexData + static_cast<std::size_t>(a_index) * stride);
				return a_toRoot * RE::NiPoint3{ p[0], p[1], p[2] };
			};
			for (std::uint32_t t = 0; t < counts.triangleCount; ++t) {
				const auto* index = mesh->rawIndexData + static_cast<std::size_t>(t) * 3;
				if (index[0] < counts.vertexCount && index[1] < counts.vertexCount && index[2] < counts.vertexCount) {
					a_visit(vertex(index[0]), vertex(index[1]), vertex(index[2]));
				}
			}
			return true;
		}

		struct Box
		{
			RE::NiPoint3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
			RE::NiPoint3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

			void Add(const RE::NiPoint3& a_point)
			{
				min = { std::min(min.x, a_point.x), std::min(min.y, a_point.y), std::min(min.z, a_point.z) };
				max = { std::max(max.x, a_point.x), std::max(max.y, a_point.y), std::max(max.z, a_point.z) };
			}
			[[nodiscard]] bool Empty() const { return min.x > max.x; }
		};

		struct Leaf
		{
			RE::NiPointer<RE::NiAVObject> node;
			Box                           box;    // its shapes, in the door's space (Geometry::upright)
			RE::NiPoint3                  pivot;  // its origin there: what its animation turns it around
			// Modelled from the front only: a load door's back faces the other cell and was never
			// seen until a doorway showed it swung open (Markarth's Dwemer doors, empty but for their
			// carvings' rims).
			bool oneSided = false;
		};

		// How far open (0 shut .. 1 open) a leaf stands through one of its door's own animations,
		// sampled evenly over its length.
		constexpr std::size_t kMotionSamples = 64;
		struct Motion
		{
			float              seconds = 0.0f;
			std::vector<float> amount;
		};

		struct Geometry
		{
			RE::NiPointer<RE::NiAVObject> root;  // the 3D measured; a reloaded door is measured anew
			// The door's "Open" and "Close" animations, as a door opened by the game plays them.
			Motion opening, shutting;
			Box                           panel;  // the leaves; along the thin axis, their faces
			bool                          thinX = false;
			bool                          measured = false;  // false: the base object's bounds
			// Model space to the door's space, where every box above is: the model turned so that its
			// z is up (Upright).
			RE::NiMatrix3 upright;
			std::vector<Leaf>             leaves;  // the root itself when nothing is animated
			// Shapes the leaves don't move, with their extent: a frame stays, the box behind the
			// panel goes with the panel (Open).
			std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, Box>> fixed;
		};

		std::unordered_map<RE::FormID, Geometry>   geometries;  // under geometryLock

		// A model isn't always made standing: Castle Dour's doors (SDoorJail02Load.nif) are modelled
		// lying down, their references turned 90 degrees about x to stand them up. Everything here
		// takes z as up and the panel as across x or y; measured in the model's own axes, such a
		// door swung its leaves about the wrong axis and was mapped turned 90 degrees. The axes are
		// put in the order that makes the model's upward axis z (a turn by quarters).
		RE::NiMatrix3 Upright(RE::FormID a_door, RE::NiAVObject* a_root)
		{
			const auto* closed = ClosedLocal(a_door, a_root);
			const auto& local = closed ? closed->rotate : a_root->local.rotate;
			const auto  placed = a_root->parent ? a_root->parent->world.rotate * local : local;
			const auto  up = placed.Transpose() * RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
			const float along[3] = { up.x, up.y, up.z };
			int         axis = 2;
			for (int i = 0; i < 2; ++i) {
				if (std::abs(along[i]) > std::abs(along[axis])) {
					axis = i;
				}
			}
			const float   sign = along[axis] >= 0.0f ? 1.0f : -1.0f;
			RE::NiMatrix3 upright({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });
			upright.entry[0][(axis + 1) % 3] = sign;  // the sign twice: still a turn, not a mirror
			upright.entry[1][(axis + 2) % 3] = 1.0f;
			upright.entry[2][axis] = sign;
			return upright;
		}

		RE::NiTransform Turned(const RE::NiMatrix3& a_rotate, const RE::NiTransform& a_transform)
		{
			RE::NiTransform turn;
			turn.rotate = a_rotate;
			return turn * a_transform;
		}

		// NiQuatTransform, which CommonLib only declares: an interpolator's value.
		struct QuatTransform
		{
			RE::NiPoint3     translate;
			RE::NiQuaternion rotate;
			float            scale;
		};
		static_assert(sizeof(QuatTransform) == 0x20);

		// For a door whose mesh has no animation: the game never moves it, so there is nothing to
		// copy. Eased both ways over the length of a common door's.
		Motion StillDoorMotion(bool a_opening)
		{
			Motion motion{ 1.0f, std::vector<float>(kMotionSamples) };
			for (std::size_t i = 0; i < kMotionSamples; ++i) {
				const float t = static_cast<float>(i) / static_cast<float>(kMotionSamples - 1);
				const float eased = 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * t);
				motion.amount[i] = a_opening ? eased : 1.0f - eased;
			}
			return motion;
		}

		// The door's own "Open" and "Close" animations (its NiControllerManager's sequences), as the
		// first of a_leaves that "Open" moves: how far it has turned from shut, as a share of how far
		// "Open" turns it. A leaf that slides instead of turning, by how far it has moved. A leaf
		// "Open" leaves where it is (a latch, a frame piece the controller also targets) has no
		// share to give: every amount divided by its zero movement.
		std::optional<std::pair<Motion, Motion>> OwnMotion(RE::NiAVObject* a_root, const std::vector<RE::NiAVObject*>& a_leaves)
		{
			RE::NiControllerManager* manager = nullptr;
			for (auto* controller = a_root->GetControllers(); controller && !manager; controller = controller->next.get()) {
				manager = netimmerse_cast<RE::NiControllerManager*>(controller);
			}
			auto* open = manager ? manager->GetSequenceByName("Open") : nullptr;
			auto* close = manager ? manager->GetSequenceByName("Close") : nullptr;
			if (!open || !close) {
				return std::nullopt;
			}
			// Only a transform controller's: a node can also carry a visibility or a colour track,
			// whose interpolator fills no transform.
			const auto interpolatorOf = [](RE::NiControllerSequence* a_sequence, RE::NiAVObject* a_leaf) -> RE::NiInterpolator* {
				for (std::uint32_t i = 0; i < a_sequence->arraySize; ++i) {
					const auto& tag = a_sequence->idTagArray.data()[i];
					if (tag.avObjectName == a_leaf->name && tag.ctlrType == "NiTransformController") {
						return a_sequence->interpArray.data()[i].interpolator.get();
					}
				}
				return nullptr;
			};
			const auto at = [](RE::NiInterpolator* a_interpolator, float a_time) {
				QuatTransform value{};
				a_interpolator->Update1(a_time, nullptr, reinterpret_cast<RE::NiQuatTransform&>(value));
				return value;
			};
			for (auto* leaf : a_leaves) {
				auto* openInterpolator = interpolatorOf(open, leaf);
				auto* closeInterpolator = interpolatorOf(close, leaf);
				if (!openInterpolator || !closeInterpolator) {
					continue;
				}
				const auto shut = at(openInterpolator, open->beginKeyTime);
				const auto turned = [&](const QuatTransform& a_value) {
					const float dot = std::abs(a_value.rotate.w * shut.rotate.w + a_value.rotate.x * shut.rotate.x + a_value.rotate.y * shut.rotate.y + a_value.rotate.z * shut.rotate.z);
					return 2.0f * std::acos(std::min(dot, 1.0f));
				};
				const auto  moved = [&](const QuatTransform& a_value) { return a_value.translate.GetDistance(shut.translate); };
				const auto  opened = at(openInterpolator, open->endKeyTime);
				const bool  slides = turned(opened) < RE::deg_to_rad(1.0f);
				const float full = slides ? moved(opened) : turned(opened);
				constexpr float kStill = 1.0f;  // units: under this the leaf doesn't slide either
				if (slides && full < kStill) {
					continue;
				}
				const auto sample = [&](RE::NiControllerSequence* a_sequence, RE::NiInterpolator* a_interpolator) {
					Motion motion{ (a_sequence->endKeyTime - a_sequence->beginKeyTime) / a_sequence->frequency, std::vector<float>(kMotionSamples) };
					for (std::size_t i = 0; i < kMotionSamples; ++i) {
						const float t = static_cast<float>(i) / static_cast<float>(kMotionSamples - 1);
						const auto  value = at(a_interpolator, a_sequence->beginKeyTime + t * (a_sequence->endKeyTime - a_sequence->beginKeyTime));
						motion.amount[i] = (slides ? moved(value) : turned(value)) / full;
					}
					return motion;
				};
				return std::pair{ sample(open, openInterpolator), sample(close, closeInterpolator) };
			}
			return std::nullopt;
		}

		float AmountAt(const Motion& a_motion, float a_t)
		{
			const float at = std::clamp(a_t, 0.0f, 1.0f) * static_cast<float>(a_motion.amount.size() - 1);
			const auto  i = std::min(static_cast<std::size_t>(at), a_motion.amount.size() - 2);
			const float f = at - static_cast<float>(i);
			return a_motion.amount[i] + (a_motion.amount[i + 1] - a_motion.amount[i]) * f;
		}

		Geometry Measure(RE::TESObjectREFR* a_door, RE::NiAVObject* a_root)
		{
			const auto id = a_door->GetFormID();
			Geometry   geometry;
			geometry.root.reset(a_root);
			geometry.upright = Upright(id, a_root);
			// a_object's transform in the door's space.
			const auto toDoor = [&](const RE::NiAVObject* a_object) { return Turned(geometry.upright, ToRoot(id, a_object, a_root)); };
			const auto animated = AnimatedNodes(a_root);
			const auto leafOf = [&](const RE::NiAVObject* a_object) -> int {
				for (auto* node = a_object; node && node != a_root; node = node->parent) {
					if (const auto it = std::ranges::find(animated, node); it != animated.end()) {
						return static_cast<int>(it - animated.begin());
					}
				}
				return animated.empty() ? 0 : -1;
			};
			if (animated.empty()) {
				geometry.leaves.push_back({ RE::NiPointer<RE::NiAVObject>(a_root), {}, {} });
			}
			for (auto* node : animated) {
				geometry.leaves.push_back({ RE::NiPointer<RE::NiAVObject>(node), {}, toDoor(node).translate });
			}
			if (const auto own = OwnMotion(a_root, animated)) {
				std::tie(geometry.opening, geometry.shutting) = *own;
			} else {
				geometry.opening = StillDoorMotion(true);
				geometry.shutting = StillDoorMotion(false);
			}
			logger::info("portal: door {:08X}: opens over {:.2f} s, shuts over {:.2f} s{}", id, geometry.opening.seconds, geometry.shutting.seconds,
				animated.empty() ? " (not animated)" : "");

			struct Triangle
			{
				RE::NiPoint3 a, b, c;
				int          leaf;
			};
			std::vector<Triangle> panelTriangles;
			bool                  complete = true;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_shape) {
				const int  leaf = leafOf(a_shape);
				Box        box;
				complete &= ForEachTriangle(a_shape, toDoor(a_shape), [&](const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c) {
					box.Add(a);
					box.Add(b);
					box.Add(c);
					if (leaf >= 0) {
						panelTriangles.push_back({ a, b, c, leaf });
					}
				});
				if (box.Empty()) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				if (leaf >= 0) {
					auto& leafBox = geometry.leaves[leaf].box;
					leafBox.Add(box.min);
					leafBox.Add(box.max);
				} else {
					geometry.fixed.emplace_back(RE::NiPointer<RE::BSGeometry>(a_shape), box);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			for (const auto& leaf : geometry.leaves) {
				if (!leaf.box.Empty()) {
					geometry.panel.Add(leaf.box.min);
					geometry.panel.Add(leaf.box.max);
				}
			}
			if (!complete || geometry.panel.Empty()) {
				const auto& bound = a_door->GetBaseObject()->boundData;
				geometry.panel = {};
				geometry.panel.Add(geometry.upright * RE::NiPoint3{ static_cast<float>(bound.boundMin.x), static_cast<float>(bound.boundMin.y), static_cast<float>(bound.boundMin.z) });
				geometry.panel.Add(geometry.upright * RE::NiPoint3{ static_cast<float>(bound.boundMax.x), static_cast<float>(bound.boundMax.y), static_cast<float>(bound.boundMax.z) });
				geometry.thinX = (geometry.panel.max.x - geometry.panel.min.x) < (geometry.panel.max.y - geometry.panel.min.y);
				for (auto& leaf : geometry.leaves) {
					leaf.box = geometry.panel;
				}
				geometry.fixed.clear();
				logger::info("portal: door {:08X}: mesh not readable, using its bounds", id);
				return geometry;
			}
			geometry.measured = true;
			geometry.thinX = (geometry.panel.max.x - geometry.panel.min.x) < (geometry.panel.max.y - geometry.panel.min.y);

			// The faces: surface facing along the thin axis, by where it is on that axis (1 unit
			// steps); the outermost steps holding a good share of the most are the panel's faces.
			std::map<int, float> facing;
			for (const auto& t : panelTriangles) {
				const auto  normal = (t.b - t.a).Cross(t.c - t.a);  // length: twice the area
				const float along = 0.5f * std::abs(geometry.thinX ? normal.x : normal.y);
				const float at = (geometry.thinX ? t.a.x + t.b.x + t.c.x : t.a.y + t.b.y + t.c.y) / 3.0f;
				facing[static_cast<int>(std::floor(at))] += along;
			}
			float most = 0.0f;
			for (const auto& [at, area] : facing) {
				most = std::max(most, area);
			}
			int front = INT_MAX, back = INT_MIN;
			for (const auto& [at, area] : facing) {
				if (area >= 0.25f * most) {
					front = std::min(front, at);
					back = std::max(back, at);
				}
			}
			// A leaf whose surface faces one way along the thin axis only (a tenth the other way at
			// most: a carving's rim, a handle's back).
			std::vector<std::array<float, 2>> sides(geometry.leaves.size());
			for (const auto& t : panelTriangles) {
				const auto  normal = (t.b - t.a).Cross(t.c - t.a);
				const float along = geometry.thinX ? normal.x : normal.y;
				sides[t.leaf][along >= 0.0f ? 0 : 1] += std::abs(along);
			}
			for (std::size_t i = 0; i < geometry.leaves.size(); ++i) {
				geometry.leaves[i].oneSided = std::min(sides[i][0], sides[i][1]) < 0.1f * std::max(sides[i][0], sides[i][1]);
			}
			if (front <= back) {
				auto& low = geometry.thinX ? geometry.panel.min.x : geometry.panel.min.y;
				auto& high = geometry.thinX ? geometry.panel.max.x : geometry.panel.max.y;
				low = std::max(low, static_cast<float>(front));
				high = std::min(high, static_cast<float>(back + 1));
			}
			logger::info("portal: door {:08X}: panel ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}), {} leaf/leaves ({} without a back), {} fixed shape(s)", id,
				geometry.panel.min.x, geometry.panel.min.y, geometry.panel.min.z, geometry.panel.max.x, geometry.panel.max.y, geometry.panel.max.z,
				geometry.leaves.size(), std::ranges::count_if(geometry.leaves, [](const Leaf& a_leaf) { return a_leaf.oneSided; }), geometry.fixed.size());
			return geometry;
		}

		// The door's geometry, measured the first time it's asked for with this 3D.
		Geometry& GeometryOf(RE::TESObjectREFR* a_door)
		{
			auto* root = a_door->Get3D();
			auto& geometry = geometries[a_door->GetFormID()];
			if (geometry.root.get() != root) {
				geometry = Measure(a_door, root);
			}
			return geometry;
		}
	}

	// The door's world transform as if it were closed.
	RE::NiTransform ClosedWorld(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D)
	{
		std::scoped_lock lock(geometryLock);
		const auto*      closed = ClosedLocal(a_door->GetFormID(), a_door3D);
		if (!closed) {
			return a_door3D->world;
		}
		return a_door3D->parent ? a_door3D->parent->world * *closed : *closed;
	}

	// Where the door stands in its own cell. A cell merged into another space (render-merge) has
	// its whole 3D moved, and the door's world transform with it; the door pair maps between the
	// cells' own coordinates.
	// Its cell's 3D, when the door hangs under it (it may not: a persistent door's 3D borrowed
	// elsewhere).
	RE::NiAVObject* CellNodeAbove(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D)
	{
		auto* cell = a_door->GetParentCell();
		auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
		auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
		for (auto* node = a_door3D->parent; cell3D && node; node = node->parent) {
			if (node == cell3D) {
				return cell3D;
			}
		}
		return nullptr;
	}

	RE::NiTransform Placement(RE::TESObjectREFR* a_door)
	{
		auto*      door3D = a_door->Get3D();
		const auto closed = ClosedWorld(a_door, door3D);
		auto*      cell3D = CellNodeAbove(a_door, door3D);
		return cell3D ? cell3D->world.Invert() * closed : closed;
	}

	// The door's space (Geometry::upright) to its cell's own coordinates.
	RE::NiTransform DoorFrame(RE::TESObjectREFR* a_door)
	{
		RE::NiTransform upright;
		{
			std::scoped_lock lock(geometryLock);
			upright.rotate = GeometryOf(a_door).upright;
		}
		return Placement(a_door) * upright.Invert();
	}

	// The cell's own coordinates to where it is drawn: Placement undone.
	RE::NiTransform ToShown(RE::TESObjectREFR* a_door)
	{
		auto* door3D = a_door->Get3D();
		auto* cell3D = door3D ? CellNodeAbove(a_door, door3D) : nullptr;
		return cell3D ? cell3D->world : RE::NiTransform{};
	}

	void Swing(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, float a_amount, RE::FormID a_settings)
	{
		const float degrees = SwingDegrees(a_settings);
		auto* door3D = a_door ? a_door->Get3D() : nullptr;
		if (!door3D || !a_door->GetBaseObject()) {
			return;
		}
		const auto id = a_door->GetFormID();
		std::scoped_lock lock(geometryLock);
		const auto&      geometry = GeometryOf(a_door);
		auto&            swung = swungClosed[id];
		if (!swung.empty() && (swung.size() != geometry.leaves.size() || swung.front().node != geometry.leaves.front().node)) {
			// The door's 3D was reloaded: its new leaves are closed. The old ones are put back:
			// that 3D may live on as what the next door of its model is copied from (Pristine).
			for (auto& leaf : swung) {
				leaf.node->local = leaf.closedLocal;
				Pristine::MovedBack(leaf.node.get());
			}
			swung.clear();
		}
		if (swung.empty()) {
			for (const auto& leaf : geometry.leaves) {
				swung.push_back({ leaf.node, leaf.node->local });
				Pristine::Moved(leaf.node.get(), leaf.node->local);
			}
			// Swung open, a leaf shows its back to whoever looks through the doorway, from either
			// side. Without one, its front is drawn there too; the lighting shader turns the normals
			// of faces seen from behind.
			auto& twoSided = swungTwoSided[id];
			for (const auto& leaf : geometry.leaves) {
				if (!leaf.oneSided) {
					continue;
				}
				RE::BSVisit::TraverseScenegraphGeometries(leaf.node.get(), [&](RE::BSGeometry* a_shape) {
					auto* property = a_shape->GetGeometryRuntimeData().shaderProperty.get();
					if (property->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided)) {
						Pristine::TwoSided(property);
						twoSided.emplace_back(property);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}
		const auto closedWorld = ClosedWorld(a_door, door3D);
		const auto room = geometry.upright * (closedWorld.rotate.Transpose() * a_room);
		RE::NiTransform upright;
		upright.rotate = geometry.upright;
		const bool wideX = !geometry.thinX;
		for (std::size_t i = 0; i < geometry.leaves.size() && i < swung.size(); ++i) {
			const auto& leaf = geometry.leaves[i];
			auto*       node = swung[i].node.get();
			// The hinge is the leaf's end its animation turns it around (its origin), or for a door
			// not animated, the low end.
			const float low = wideX ? leaf.box.min.x : leaf.box.min.y;
			const float high = wideX ? leaf.box.max.x : leaf.box.max.y;
			const float pivot = wideX ? leaf.pivot.x : leaf.pivot.y;
			const bool  hingeHigh = node != door3D && std::abs(pivot - high) < std::abs(pivot - low);
			const float        hingeEnd = hingeHigh ? high : low;
			const float        span = (high - low) * (hingeHigh ? -1.0f : 1.0f);
			const float        thinMiddle = wideX ? 0.5f * (leaf.box.min.y + leaf.box.max.y) : 0.5f * (leaf.box.min.x + leaf.box.max.x);
			const RE::NiPoint3 hinge = wideX ? RE::NiPoint3{ hingeEnd, thinMiddle, 0.0f } : RE::NiPoint3{ thinMiddle, hingeEnd, 0.0f };
			const RE::NiPoint3 free = wideX ? RE::NiPoint3{ span, 0.0f, 0.0f } : RE::NiPoint3{ 0.0f, span, 0.0f };
			const float        sign = (-free.y * room.x + free.x * room.y) >= 0.0f ? 1.0f : -1.0f;  // the free end turns towards the room
			const float        angle = sign * a_amount * RE::deg_to_rad(degrees);
			const float        c = std::cos(angle), s = std::sin(angle);
			RE::NiTransform    turn;  // in the door's space
			turn.rotate = RE::NiMatrix3({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			turn.translate = hinge - turn.rotate * hinge;
			if (node == door3D) {
				node->local = swung[i].closedLocal * upright.Invert() * turn * upright;
			} else {
				// The leaf's parent, in the door's space: the turn is carried into the leaf's own frame.
				const auto parent = upright * (node->parent ? ToRoot(id, node->parent, door3D) : RE::NiTransform{});
				node->local = parent.Invert() * turn * parent * swung[i].closedLocal;
			}
		}
		RE::NiUpdateData update;
		door3D->Update(update);
	}

	float SwingAmount(RE::TESObjectREFR* a_door, float a_t, bool a_opening)
	{
		std::scoped_lock lock(geometryLock);
		const auto&      geometry = GeometryOf(a_door);
		// a_t is how far open the swing has got, so shutting runs it from 1 down to 0: "Close" is
		// played from its end backwards in a_t, forwards in time.
		return a_opening ? AmountAt(geometry.opening, a_t) : AmountAt(geometry.shutting, 1.0f - a_t);
	}

	float SwingSeconds(RE::TESObjectREFR* a_door, bool a_opening)
	{
		std::scoped_lock lock(geometryLock);
		const auto&      geometry = GeometryOf(a_door);
		return (a_opening ? geometry.opening : geometry.shutting).seconds;
	}

	void Unswing(RE::FormID a_door)
	{
		std::scoped_lock lock(geometryLock);
		const auto       it = swungClosed.find(a_door);
		if (it == swungClosed.end()) {
			return;
		}
		for (auto& leaf : it->second) {
			leaf.node->local = leaf.closedLocal;
			Pristine::MovedBack(leaf.node.get());
		}
		for (auto& property : swungTwoSided[a_door]) {
			Pristine::OneSided(property.get());
		}
		swungTwoSided.erase(a_door);
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		auto* door3D = door ? door->Get3D() : nullptr;
		swungClosed.erase(it);
		if (door3D) {
			RE::NiUpdateData update;
			door3D->Update(update);
		}
	}

	void ForgetUnloaded()
	{
		std::scoped_lock lock(geometryLock);
		std::erase_if(geometries, [](const auto& a_entry) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_entry.first);
			return !door || door->Get3D() != a_entry.second.root.get();
		});
	}

	namespace
	{
		// A box in the door's space, placed like the door (see Corners for the bit order).
		std::array<RE::NiPoint3, 8> CornersOf(RE::TESObjectREFR* a_door, const Box& a_box)
		{
			const auto                  world = DoorFrame(a_door);
			std::array<RE::NiPoint3, 8> corners;
			for (int i = 0; i < 8; ++i) {
				const RE::NiPoint3 local{ i & 1 ? a_box.max.x : a_box.min.x, i & 2 ? a_box.max.y : a_box.min.y, i & 4 ? a_box.max.z : a_box.min.z };
				corners[i] = world * local;
			}
			return corners;
		}
	}

	std::array<RE::NiPoint3, 8> Corners(RE::TESObjectREFR* a_door)
	{
		Box box;
		{
			std::scoped_lock lock(geometryLock);
			box = GeometryOf(a_door).panel;
		}
		return CornersOf(a_door, box);
	}

	RE::NiPoint3 Center(RE::TESObjectREFR* a_door)
	{
		RE::NiPoint3 sum;
		for (const auto& c : Corners(a_door)) {
			sum += c;
		}
		return sum / 8.0f;
	}

	std::array<RE::NiPoint3, 4> Quad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel, bool a_wholeMesh)
	{
		bool thinX;
		Box  box;
		{
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			thinX = geometry.thinX;
			box = geometry.panel;
			if (a_wholeMesh) {
				// Across and up only: the faces stay the panel's.
				for (const auto& [shape, extent] : geometry.fixed) {
					(thinX ? box.min.y : box.min.x) = std::min(thinX ? box.min.y : box.min.x, thinX ? extent.min.y : extent.min.x);
					(thinX ? box.max.y : box.max.x) = std::max(thinX ? box.max.y : box.max.x, thinX ? extent.max.y : extent.max.x);
					box.min.z = std::min(box.min.z, extent.min.z);
					box.max.z = std::max(box.max.z, extent.max.z);
				}
			}
		}
		const auto c = CornersOf(a_door, box);
		const int  thinBit = thinX ? 1 : 2;
		const int  wideBit = thinX ? 2 : 1;
		RE::NiPoint3 outward = c[thinBit] - c[0];
		outward.Unitize();
		const bool maxSide = (a_viewer - (c[0] + c[7]) * 0.5f).Dot(outward) > 0.0f;
		// The panel's max face is the viewer's when maxSide; the quad goes 2 units off the chosen face,
		// away from the panel.
		const bool maxFace = maxSide != a_behindPanel;
		const int  face = maxFace ? thinBit : 0;
		const auto shift = outward * (maxFace ? 2.0f : -2.0f);
		return { c[face] + shift, c[face | wideBit] + shift, c[face | 4] + shift, c[face | wideBit | 4] + shift };
	}

	std::array<RE::NiPoint3, 4> ShownQuad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, bool a_behindPanel, bool a_wholeMesh)
	{
		const auto toShown = ToShown(a_door);
		auto       quad = Quad(a_door, toShown.Invert() * a_viewer, a_behindPanel, a_wholeMesh);
		for (auto& corner : quad) {
			corner = toShown * corner;
		}
		return quad;
	}

	RE::NiPoint3 ShownCenter(RE::TESObjectREFR* a_door)
	{
		return ToShown(a_door) * Center(a_door);
	}

	PanelSize Size(RE::TESObjectREFR* a_door)
	{
		Box   box;
		bool  thinX;
		{
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			box = geometry.panel;
			thinX = geometry.thinX;
		}
		// In its cell's own coordinates: a street merged into an interior is drawn scaled
		// (PlacementOf), and its door with it.
		const float scale = Placement(a_door).scale;
		const float x = box.max.x - box.min.x, y = box.max.y - box.min.y;
		return { 0.5f * scale * (thinX ? y : x), 0.5f * scale * (thinX ? x : y), 0.5f * scale * (box.max.z - box.min.z) };
	}

	namespace
	{
		// The shapes standing behind the panel, away from the door's own side (where its partner's
		// marker puts whoever comes through): the box that hides the void behind a closed door.
		std::vector<RE::NiPointer<RE::BSGeometry>> BehindPanel(RE::TESObjectREFR* a_door)
		{
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			auto  partner = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
			auto* partnerTeleport = partner ? partner->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
			if (!partnerTeleport || !partnerTeleport->teleportData) {
				return {};
			}
			const auto  front = DoorFrame(a_door).Invert() * partnerTeleport->teleportData->position;
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			if (!geometry.measured) {
				return {};
			}
			const auto  along = [&](const RE::NiPoint3& a_point) { return geometry.thinX ? a_point.x : a_point.y; };
			const float low = along(geometry.panel.min), high = along(geometry.panel.max);
			const bool  frontHigh = along(front) > 0.5f * (low + high);
			std::vector<RE::NiPointer<RE::BSGeometry>> behind;
			for (const auto& [shape, box] : geometry.fixed) {
				// Reaching out behind the panel and nowhere in front of its face (the dragon door's box
				// starts inside the panel's thickness); a frame reaches out on both sides.
				const bool isBehind = frontHigh ? along(box.max) <= high + 1.0f && along(box.min) < low : along(box.min) >= low - 1.0f && along(box.max) > high;
				if (isBehind) {
					behind.push_back(shape);
				}
			}
			return behind;
		}
	}

	namespace
	{
		std::mutex                            correctionsLock;
		std::unordered_map<RE::FormID, float> corrections;  // under correctionsLock
		std::unordered_map<RE::FormID, float> swingDegrees;  // under correctionsLock
		std::unordered_set<RE::FormID>        portalsOff;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        opensInward;  // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        alwaysOpen;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_set<RE::FormID>        farPicture;   // under correctionsLock, keyed by CorrectionDoor
		std::unordered_map<RE::FormID, RE::NiPoint3> shifts;  // under correctionsLock
		bool                                  correctionsLoaded = false;

		// The doors the mod ships with. Never written: an update replaces it, which would take the
		// player's own tunings with it if they were kept here too.
		std::filesystem::path DefaultsFile()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doors.ini";
		}

		// The doors tuned in game. No release ships it, so no update replaces it.
		std::filesystem::path TuningsFile()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doors_User.ini";
		}

		// "Skyrim.esm|01A6F5": the same door whatever the load order.
		std::string CorrectionKey(RE::FormID a_door)
		{
			auto* form = RE::TESForm::LookupByID(a_door);
			auto* file = form ? form->GetFile(0) : nullptr;
			if (!file) {
				return std::format("{:08X}", a_door);
			}
			const auto local = file->IsLight() ? (a_door & 0xFFF) : (a_door & 0xFFFFFF);
			return std::format("{}|{:06X}", file->GetFilename(), local);
		}

		// The file's lines by section, read by hand: the system's own reader (GetPrivateProfileSection)
		// gives up to 32767 characters of a section and drops the rest without a word, the last
		// entry cut in the middle. A patcher's list for a whole load order is longer than that.
		using Sections = std::vector<std::pair<std::wstring, std::vector<std::wstring>>>;
		Sections ReadSections(const std::filesystem::path& a_file)
		{
			std::ifstream     stream(a_file, std::ios::binary);
			const std::string bytes{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
			std::wstring      text;
			if (bytes.starts_with("\xFF\xFE")) {
				text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
			} else {
				// As the system's writer (WritePrivateProfileString, SaveCorrection) leaves it.
				text.resize(bytes.size());
				text.resize(::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), static_cast<int>(text.size())));
			}
			const auto trimmed = [](std::wstring_view a_text) {
				const auto first = a_text.find_first_not_of(L" \t\r");
				const auto last = a_text.find_last_not_of(L" \t\r");
				return first == std::wstring_view::npos ? std::wstring_view{} : a_text.substr(first, last - first + 1);
			};
			Sections sections;
			for (std::size_t at = 0; at < text.size();) {
				const auto end = std::min(text.find(L'\n', at), text.size());
				const auto line = trimmed(std::wstring_view(text).substr(at, end - at));
				at = end + 1;
				if (line.empty() || line.front() == L';') {
					continue;
				}
				if (line.front() == L'[' && line.back() == L']') {
					sections.emplace_back(line.substr(1, line.size() - 2), std::vector<std::wstring>{});
				} else if (!sections.empty()) {
					sections.back().second.emplace_back(line);
				}
			}
			return sections;
		}

		// Every section's entries are Plugin.esp|localID=value; a_use gets the door and the value. An
		// empty a_section: the entries of every section.
		void ForEachEntry(const Sections& a_sections, std::wstring_view a_section, const std::function<void(RE::FormID, const wchar_t*)>& a_use)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (const auto& [name, entries] : a_sections) {
				if (!a_section.empty() && ::_wcsnicmp(name.c_str(), a_section.data(), std::max(name.size(), a_section.size())) != 0) {
					continue;
				}
				for (const auto& entry : entries) {
					const auto equals = entry.find(L'=');
					const auto bar = entry.find(L'|');
					if (equals == std::wstring::npos || bar == std::wstring::npos || bar > equals) {
						continue;
					}
					const std::string file = Paths::Narrow(std::wstring_view(entry).substr(0, bar));
					const auto        local = static_cast<RE::FormID>(std::wcstoul(entry.substr(bar + 1, equals - bar - 1).c_str(), nullptr, 16));
					if (const auto id = handler->LookupFormID(local, file)) {
						a_use(id, entry.c_str() + equals + 1);
					}
				}
			}
		}

		// Under correctionsLock.
		void Apply(const Sections& a_sections)
		{
			const auto each = [&](std::wstring_view a_section, const std::function<void(RE::FormID, const wchar_t*)>& a_use) {
				ForEachEntry(a_sections, a_section, a_use);
			};
			each(L"DoorTurn", [](RE::FormID a_door, const wchar_t* a_value) {
				if (const float degrees = std::wcstof(a_value, nullptr); degrees != 0.0f) {
					corrections[a_door] = degrees;
				}
			});
			// The older [DoorOpen] and [DoorShift] were made against doors measured by their bounds:
			// the hinge guessed from them, the shift mostly making up for the bounds' depth. Against
			// the measured panel neither means the same, so they are left unread, as is [DoorHinge],
			// the other end of the leaf for a hinge, which a smaller swing angle made unneeded.
			each(L"DoorSwing", [](RE::FormID a_door, const wchar_t* a_value) { swingDegrees[a_door] = std::wcstof(a_value, nullptr); });
			// [DoorPortal] =0: the pair's portal switched off.
			each(L"DoorPortal", [](RE::FormID a_door, const wchar_t* a_value) {
				if (std::wcstol(a_value, nullptr, 10) == 0) {
					portalsOff.insert(a_door);
				}
			});
			// [DoorOpensIn], [DoorAlwaysOpen] and [DoorFarPicture] =1.
			for (const auto& [section, set] : { std::pair{ L"DoorOpensIn", &opensInward }, std::pair{ L"DoorAlwaysOpen", &alwaysOpen }, std::pair{ L"DoorFarPicture", &farPicture } }) {
				each(section, [set](RE::FormID a_door, const wchar_t* a_value) {
					if (std::wcstol(a_value, nullptr, 10) != 0) {
						set->insert(a_door);
					}
				});
			}
			// [DoorOffset] =x,y,z
			each(L"DoorOffset", [](RE::FormID a_door, const wchar_t* a_value) {
				RE::NiPoint3 shift;
				if (std::swscanf(a_value, L"%f,%f,%f", &shift.x, &shift.y, &shift.z) == 3) {
					shifts[a_door] = shift;
				}
			});
		}

		void LoadCorrections()
		{
			if (std::exchange(correctionsLoaded, true)) {
				return;
			}
			Apply(ReadSections(DefaultsFile()));
			// A door the player tuned takes none of the shipped settings: an entry missing from the
			// tunings means the built-in default (a portal switched back on, a door no longer shifted),
			// not the shipped value. SaveCorrection always writes [DoorPortal], so a tuned door is in
			// the file even when all its settings are the built-in ones.
			const auto              tunings = ReadSections(TuningsFile());
			std::vector<RE::FormID> tuned;
			ForEachEntry(tunings, {}, [&](RE::FormID a_door, const wchar_t*) {
				if (std::ranges::find(tuned, a_door) == tuned.end()) {
					tuned.push_back(a_door);
				}
			});
			for (const auto door : tuned) {
				corrections.erase(door);
				swingDegrees.erase(door);
				portalsOff.erase(door);
				opensInward.erase(door);
				alwaysOpen.erase(door);
				farPicture.erase(door);
				shifts.erase(door);
			}
			Apply(tunings);
			logger::info("portal: {} door turn(s), {} door shift(s), {} swing angle(s), {} portal(s) off, {} opening inward, {} always open, {} gate(s) with a picture from afar loaded; {} door(s) tuned by the player",
				corrections.size(), shifts.size(), swingDegrees.size(), portalsOff.size(), opensInward.size(), alwaysOpen.size(), farPicture.size(), tuned.size());
		}

		RE::NiTransform TurnAround(float a_radians, const RE::NiPoint3& a_center)
		{
			const float     c = std::cos(a_radians), s = std::sin(a_radians);
			RE::NiTransform turn;
			turn.rotate = RE::NiMatrix3({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			turn.translate = a_center - turn.rotate * a_center;
			return turn;
		}
	}

	RE::TESObjectREFR* CorrectionDoor(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked)
	{
		// A persistent street door has no parent cell while the player is indoors (a save loaded
		// there): told by its worldspace, as the manager's Outdoors does. Taken for indoors, the
		// menu stored a door's settings under the pair's other door, where the swing never read them.
		const auto outdoors = [](RE::TESObjectREFR* a_door) {
			auto* cell = a_door->GetParentCell();
			return cell ? !cell->IsInteriorCell() : a_door->GetWorldspace() != nullptr;
		};
		const bool sourceOut = outdoors(a_source);
		const bool linkedOut = outdoors(a_linked);
		return CorrectionDoor(a_source->GetFormID(), sourceOut, a_linked->GetFormID(), linkedOut) == a_source->GetFormID() ? a_source : a_linked;
	}

	RE::FormID CorrectionDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors)
	{
		if (a_sourceOutdoors != a_linkedOutdoors) {
			return a_sourceOutdoors ? a_source : a_linked;
		}
		return a_source < a_linked ? a_source : a_linked;
	}

	RE::FormID SwingingDoor(RE::FormID a_source, bool a_sourceOutdoors, RE::FormID a_linked, bool a_linkedOutdoors)
	{
		const auto key = CorrectionDoor(a_source, a_sourceOutdoors, a_linked, a_linkedOutdoors);
		return OpensInward(key) == (key == a_source) ? a_linked : a_source;
	}

	float Correction(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = corrections.find(a_door);
		return it != corrections.end() ? it->second : 0.0f;
	}

	void SetCorrection(RE::FormID a_door, float a_degrees)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		corrections[a_door] = a_degrees;
	}

	void SaveCorrection(RE::FormID a_door)
	{
		const float degrees = Correction(a_door);
		const auto  key = CorrectionKey(a_door);
		const std::wstring wkey = Paths::Widen(key);
		const auto         file = TuningsFile();
		const auto         value = std::format(L"{:.2f}", degrees);
		::WritePrivateProfileStringW(L"DoorTurn", wkey.c_str(), degrees != 0.0f ? value.c_str() : nullptr, file.c_str());
		const float swing = SwingDegrees(a_door);
		const auto  swingValue = std::format(L"{:.0f}", swing);
		::WritePrivateProfileStringW(L"DoorSwing", wkey.c_str(), swing != kSwingDegrees ? swingValue.c_str() : nullptr, file.c_str());
		const auto shift = Shift(a_door);
		const auto shiftValue = std::format(L"{:.1f},{:.1f},{:.1f}", shift.x, shift.y, shift.z);
		const bool shifted = shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f;
		::WritePrivateProfileStringW(L"DoorOffset", wkey.c_str(), shifted ? shiftValue.c_str() : nullptr, file.c_str());
		const bool off = PortalOff(a_door);
		const bool inward = OpensInward(a_door);
		const bool open = AlwaysOpen(a_door);
		const bool picture = PictureFromAfar(a_door);
		// Written even when on: it keeps the door in the file, so LoadCorrections lets none of the
		// shipped settings back in.
		::WritePrivateProfileStringW(L"DoorPortal", wkey.c_str(), off ? L"0" : L"1", file.c_str());
		::WritePrivateProfileStringW(L"DoorOpensIn", wkey.c_str(), inward ? L"1" : nullptr, file.c_str());
		::WritePrivateProfileStringW(L"DoorAlwaysOpen", wkey.c_str(), open ? L"1" : nullptr, file.c_str());
		::WritePrivateProfileStringW(L"DoorFarPicture", wkey.c_str(), picture ? L"1" : nullptr, file.c_str());
		logger::info("portal: door {} saved: turned {:.2f} degrees, opens by {:.0f} degrees{}{}{}{}", key, degrees, swing,
			off ? ", portal off" : ", portal on", inward ? ", opens inward" : "", open ? ", always open" : "", picture ? ", picture from afar" : "");
	}

	RE::NiPoint3 Shift(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = shifts.find(a_door);
		return it != shifts.end() ? it->second : RE::NiPoint3{};
	}

	void SetShift(RE::FormID a_door, const RE::NiPoint3& a_shift)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		shifts[a_door] = a_shift;
	}

	bool PortalOff(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return portalsOff.contains(a_door);
	}

	void SetPortalOff(RE::FormID a_door, bool a_off)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_off) {
			portalsOff.insert(a_door);
		} else {
			portalsOff.erase(a_door);
		}
	}

	bool OpensInward(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return opensInward.contains(a_door);
	}

	void SetOpensInward(RE::FormID a_door, bool a_inward)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_inward) {
			opensInward.insert(a_door);
		} else {
			opensInward.erase(a_door);
		}
	}

	bool AlwaysOpen(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return alwaysOpen.contains(a_door);
	}

	void SetAlwaysOpen(RE::FormID a_door, bool a_open)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_open) {
			alwaysOpen.insert(a_door);
		} else {
			alwaysOpen.erase(a_door);
		}
	}

	std::vector<RE::FormID> AlwaysOpenDoors()
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return { alwaysOpen.begin(), alwaysOpen.end() };
	}

	bool PictureFromAfar(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		return farPicture.contains(a_door);
	}

	void SetPictureFromAfar(RE::FormID a_door, bool a_picture)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		if (a_picture) {
			farPicture.insert(a_door);
		} else {
			farPicture.erase(a_door);
		}
	}

	float SwingDegrees(RE::FormID a_door)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		const auto it = swingDegrees.find(a_door);
		return it != swingDegrees.end() ? it->second : kSwingDegrees;
	}

	void SetSwingDegrees(RE::FormID a_door, float a_degrees)
	{
		std::scoped_lock lock(correctionsLock);
		LoadCorrections();
		swingDegrees[a_door] = a_degrees;
	}

	Mapping Compute(const Manager::Active& a_portal)
	{
		// The source door's teleport marker is where you appear inside and which way you face; the
		// linked door's marker is where you appear outside, facing away from the door.
		const auto yawOf = [](RE::TESObjectREFR* a_door) {
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				throw std::runtime_error(std::format("door {:08X} has no teleport marker", a_door->GetFormID()));
			}
			return teleport->teleportData->rotation.z;
		};
		// A doorway is mapped square onto the other: each door's facing is its panel's normal, the
		// marker only picks which of its two sides. Markers aren't always square to their door: the one
		// inside Solitude's gate faces 48 degrees up the street, and the outside was shown that much
		// turned; the Smelter Overseer's House's street door 17, Whiterun's shops 4 to 11, turned back
		// by hand before. (The door meshes' own rotations can't be used for this: the meshes aren't
		// placed consistently.)
		const auto facing = [](RE::TESObjectREFR* a_door, float a_markerYaw) {
			const auto corners = Corners(a_door);
			bool       thinX;
			{
				std::scoped_lock lock(geometryLock);
				thinX = GeometryOf(a_door).thinX;
			}
			const auto  normal = corners[thinX ? 1 : 2] - corners[0];
			const float yaw = std::atan2(normal.x, normal.y);
			return std::cos(yaw - a_markerYaw) >= 0.0f ? yaw : yaw + RE::NI_PI;
		};
		// A frame whose local +y is the heading `a_yaw` (Skyrim: forward = (sin, cos)), +z up.
		const auto frame = [](float a_yaw, const RE::NiPoint3& a_origin) {
			const float     s = std::sin(a_yaw), c = std::cos(a_yaw);
			RE::NiTransform t;
			t.rotate = RE::NiMatrix3({ c, s, 0.0f }, { -s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			t.translate = a_origin;
			return t;
		};
		auto*       source = Door(a_portal.sourceDoor);
		auto*       linked = Door(a_portal.linkedDoor);
		// Outside: walking into the source door = facing opposite to how you come out of it.
		const float inYaw = facing(source, yawOf(linked) + RE::NI_PI);
		const float outYaw = facing(linked, yawOf(source));
		const auto  outside = frame(inYaw, Center(source));
		const auto  inside = frame(outYaw, Center(linked));

		// A gate's two doors can be one mesh placed at different sizes: Whiterun's is 10% larger on
		// the city side. Seen through either, the other side's gateway didn't fill the one it was seen
		// in (its arch lower, its threshold higher). Mapped scaled by their ratio around the doorway,
		// the two gateways are one: the camera behind stands that much nearer (or further from) the
		// far gate, which looks the same as that side drawn larger. A street seen from a room is
		// scaled where it is merged instead (PlacementOf). Only one mesh (by its model: the two sides'
		// base doors can be two forms): two different gates'
		// heights say nothing about their scale, and a scaled mapping also moves where the player
		// and the people walking through come out. Within 1% it is the measurement's noise.
		RE::NiTransform grow;
		const auto modelOf = [](RE::TESObjectREFR* a_door) { return a_door->GetBaseObject()->As<RE::TESModel>()->GetModel(); };
		if (!source->GetParentCell()->IsInteriorCell() && !linked->GetParentCell()->IsInteriorCell() && _stricmp(modelOf(source), modelOf(linked)) == 0) {
			const float ratio = Size(linked).halfHeight / Size(source).halfHeight;
			if (std::abs(ratio - 1.0f) > 0.01f) {
				grow.scale = ratio;
			}
		}
		Mapping mapping;
		mapping.toLinked = inside * grow * outside.Invert();
		mapping.sourceCenter = Center(source);
		mapping.inward = { std::sin(inYaw), std::cos(inYaw), 0.0f };
		// The hand correction turns the inside around its own door: seen from the door that holds
		// it, the other side's picture turns; from the other side, the way back turns the opposite way.
		auto* key = CorrectionDoor(source, linked);
		if (const float degrees = Correction(key->GetFormID()); degrees != 0.0f) {
			const float turn = RE::deg_to_rad(degrees);
			if (key == source) {
				mapping.toLinked = TurnAround(turn, Center(linked)) * mapping.toLinked;
			} else {
				mapping.toLinked = mapping.toLinked * TurnAround(-turn, Center(source));
			}
		}
		// The shift moves the inside's picture in the key door's space (from there, toLinked reads
		// a point d further back; the way back adds d again).
		if (const auto shift = Shift(key->GetFormID()); shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f) {
			const float  yaw = key == source ? inYaw : outYaw + RE::NI_PI;  // walking into the key door
			const RE::NiPoint3 in{ std::sin(yaw), std::cos(yaw), 0.0f };
			const RE::NiPoint3 right{ in.y, -in.x, 0.0f };
			RE::NiTransform move;
			move.translate = right * shift.x + in * shift.y + RE::NiPoint3{ 0.0f, 0.0f, shift.z };
			if (key == source) {
				mapping.toLinked = mapping.toLinked * move.Invert();
			} else {
				mapping.toLinked = move * mapping.toLinked;
			}
		}
		// Read off the mapping itself (where heading 0 ends up), so a walker arrives facing the way
		// the doorway showed: kept as a sum next to it, a hand correction turned the two apart by
		// twice its angle.
		const auto ahead = mapping.toLinked.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
		mapping.yawDelta = std::atan2(ahead.x, ahead.y);
		const auto size = Size(source);
		mapping.halfWidth = size.halfWidth;
		mapping.halfDepth = size.halfDepth;
		mapping.halfHeight = size.halfHeight;
		return mapping;
	}

	std::optional<Mapping> TryCompute(const Manager::Active& a_portal)
	{
		const auto has3D = [](RE::FormID a_id) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			return door && door->Get3D();
		};
		if (!has3D(a_portal.sourceDoor) || !has3D(a_portal.linkedDoor)) {
			return std::nullopt;
		}
		return Compute(a_portal);
	}

	namespace
	{
		// One door can be held open twice at once: as the portal door of the space the player is in
		// and as the door they just came out of (Travel). Its original collision is recorded by the
		// first opener and restored when the last one lets go.
		struct Shared
		{
			int                                                                  opens = 0;
			int                                                                  hides = 0;
			int                                                                  passables = 0;
			int                                                                  walkables = 0;  // openers not solid (OpenState::solid)
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::COL_LAYER>> layers;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           hiddenShapes;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           behindShapes;
			// Taken out of the Havok world while the door is passable (HideBodies), and the 3D they
			// belong to: put back only into that.
			std::vector<RE::NiPointer<RE::bhkWorldObject>>                       removedBodies;
			RE::NiPointer<RE::NiAVObject>                                        bodiesOf;
			// The 3D all of the above was done to (see Rebuilt).
			RE::NiPointer<RE::NiAVObject>                                        root;
		};

		// The layer change alone didn't stop a city gate: Whiterun's gate kept the player 25 units
		// short of its doorway (in first person often just short enough for the crossing, which needs
		// the camera at the picture, not to happen), and it stopped rays as well.
		void HideBodies(RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			a_shared.bodiesOf.reset(a_door3D);
			RE::BSVisit::TraverseScenegraphObjects(a_door3D, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr;
				if (collision && collision->body && collision->body->GetWorld1()) {
					a_shared.removedBodies.emplace_back(collision->body);
					collision->body->RemoveFromCurrentWorld();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void ShowBodies(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			auto* cell = a_door ? a_door->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			// Only into the 3D they came from, still standing in an attached cell: a cell let go in the
			// meantime has taken its world apart, and a reloaded door has bodies of its own.
			if (world && a_door->Get3D() == a_shared.bodiesOf.get() && cell->cellState.get() == RE::TESObjectCELL::CellState::kAttached) {
				for (auto& body : a_shared.removedBodies) {
					if (!body->GetWorld1()) {
						body->MoveToWorld(world);
					}
				}
			}
			a_shared.removedBodies.clear();
			a_shared.bodiesOf.reset();
		}
		std::unordered_map<RE::FormID, Shared> openDoors;

		// What stands behind the panel only to hide the void there would now block the view through
		// the doorway; it went round with the panel when that was swung as a whole.
		void CullBehindPanel(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			for (auto& shape : BehindPanel(a_door)) {
				if (!shape->GetAppCulled()) {
					Pristine::Hide(shape.get());
					a_shared.behindShapes.push_back(shape);
				}
			}
		}

		void MakeWalkable(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			RE::BSVisit::TraverseScenegraphObjects(a_door3D, [&](RE::NiAVObject* a_object) {
				if (a_object->collisionObject) {
					const auto layer = a_object->GetCollisionLayer();
					if (layer != RE::COL_LAYER::kUnidentified && layer != RE::COL_LAYER::kNonCollidable) {
						a_shared.layers.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), layer);
						a_object->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
						Pristine::LayerChanged(a_object, layer);
					} else {
						logger::info("portal: door {:08X}: collision on '{}' left as it is (layer {})", a_door->GetFormID(), a_object->name.c_str(), static_cast<int>(layer));
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// The leaves only: a frame in the door's mesh fills the wall around the panel (Riften's
		// RiftenRWDoorLoad01), and the other door's frame, standing in the same place, is drawn only
		// beyond the door plane and within the panel's rectangle. Hidden too, the doorway was a hole
		// in the wall wider than the opening. Culling a node alone left the panel on screen while
		// merged; the shapes are culled one by one, as for the player's body (CameraLimit).
		void HideShapes(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			std::scoped_lock lock(geometryLock);
			for (const auto& leaf : GeometryOf(a_door).leaves) {
				RE::BSVisit::TraverseScenegraphGeometries(leaf.node.get(), [&](RE::BSGeometry* a_shape) {
					if (!a_shape->GetAppCulled()) {
						Pristine::Hide(a_shape);
						a_shared.hiddenShapes.emplace_back(a_shape);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}

		// The engine built the door's 3D anew while it was held open (disabled and enabled by a
		// script, its cell's 3D reloaded): the new panel stood in the doorway, drawn and solid, while
		// every opener took the door for open. What they asked for is done to the new 3D; the
		// recorded shapes and bodies were the old one's and went with it.
		void Rebuilt(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			// The old 3D may live on as what the next door of its model is copied from (Pristine).
			for (auto& shape : a_shared.behindShapes) {
				Pristine::Show(shape.get());
			}
			for (auto& shape : a_shared.hiddenShapes) {
				Pristine::Show(shape.get());
			}
			for (auto& [object, layer] : a_shared.layers) {
				object->SetCollisionLayer(layer);
				Pristine::LayerBack(object.get());
			}
			a_shared.behindShapes.clear();
			a_shared.layers.clear();
			a_shared.hiddenShapes.clear();
			a_shared.removedBodies.clear();
			a_shared.bodiesOf.reset();
			a_shared.root.reset(a_door3D);
			CullBehindPanel(a_door, a_shared);
			if (a_shared.walkables > 0) {
				MakeWalkable(a_door, a_door3D, a_shared);
			}
			if (a_shared.hides > 0) {
				HideShapes(a_door, a_shared);
			}
			if (a_shared.passables > 0) {
				HideBodies(a_door3D, a_shared);
			}
			logger::info("portal: door {:08X}'s 3D was rebuilt while open: opened again", a_door->GetFormID());
		}

	}

	bool IsOpen(RE::FormID a_door)
	{
		return openDoors.contains(a_door);
	}

	bool IsSwung(RE::FormID a_door)
	{
		std::scoped_lock lock(geometryLock);
		return swungClosed.contains(a_door);
	}

	std::uint32_t HiddenByNoOpener(RE::TESObjectREFR* a_door)
	{
		const auto    open = openDoors.find(a_door->GetFormID());
		std::uint32_t count = 0;
		RE::BSVisit::TraverseScenegraphGeometries(a_door->Get3D(), [&](RE::BSGeometry* a_shape) {
			const auto ours = [&](const std::vector<RE::NiPointer<RE::BSGeometry>>& a_list) { return std::ranges::contains(a_list, a_shape, &RE::NiPointer<RE::BSGeometry>::get); };
			if (a_shape->GetAppCulled() && (open == openDoors.end() || (!ours(open->second.hiddenShapes) && !ours(open->second.behindShapes)))) {
				++count;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return count;
	}

	// Standing open, not to be shut by using it: no prompt.
	bool PromptOff(RE::FormID a_door)
	{
		return Manager::Prompt(a_door) == Manager::DoorPrompt::kNone;
	}

	bool BodiesHidden(RE::FormID a_door)
	{
		const auto it = openDoors.find(a_door);
		return it != openDoors.end() && !it->second.removedBodies.empty();
	}

	// TESObjectDOOR::GetActivateText (vfunc 0x4C) builds the "Open <destination>" line for the door
	// under the crosshair. For a door held open by a portal the text is left empty here, or says
	// "Close" for one the player opened (using it shuts it, Manager's door use).
	struct ActivateTextHook
	{
		static inline std::uint32_t calls = 0;
		static bool Thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_activator, RE::BSString& a_text)
		{
			++calls;
			auto* pick = RE::CrosshairPickData::GetSingleton();
			const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
			if (target && PromptOff(target->GetFormID())) {
				a_text = "";
				return false;
			}
			const bool result = func(a_door, a_activator, a_text);
			if (target && Manager::Prompt(target->GetFormID()) == Manager::DoorPrompt::kClose) {
				auto*             settings = RE::GameSettingCollection::GetSingleton();
				auto*             open = settings->GetSetting("sOpen");
				auto*             close = settings->GetSetting("sClose");
				const std::string text = a_text.c_str();
				if (const auto at = text.find(open->GetString()); at != std::string::npos) {
					a_text = (text.substr(0, at) + close->GetString() + text.substr(at + std::strlen(open->GetString()))).c_str();
				}
			}
			return result;
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The text hook above alone left the prompt on the HUD (measured: "Open Riverwood Trader"
	// with the crosshair on a hidden, open door), so the HUD is also told, the way the engine
	// tells it when the crosshair leaves an object.
	std::uint32_t HidePrompt()
	{
		auto*      pick = RE::CrosshairPickData::GetSingleton();
		const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
		if (target && PromptOff(target->GetFormID())) {
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
			if (menu && menu->uiMovie) {
				RE::GFxValue text;
				if (menu->uiMovie->GetVariable(&text, "_root.HUDMovieBaseInstance.RolloverText.text") && text.IsString() && text.GetString()[0] != 0) {
					const RE::GFxValue args[2]{ false, "" };
					menu->uiMovie->Invoke("_root.HUDMovieBaseInstance.SetCrosshairTarget", nullptr, args, 2);
				}
			}
		}
		return ActivateTextHook::calls;
	}

	void InstallPromptHook()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectDOOR[0] };
		ActivateTextHook::func = vtable.write_vfunc(0x4C, ActivateTextHook::Thunk);
	}

	void Open(RE::TESObjectREFR* a_door, OpenState& a_state, bool a_hide, bool a_passable, bool a_solid)
	{
		auto* door3D = a_door ? a_door->Get3D() : nullptr;
		if (!door3D) {
			return;
		}
		if (const auto open = openDoors.find(a_door->GetFormID()); open != openDoors.end() && open->second.root.get() != door3D) {
			Rebuilt(a_door, door3D, open->second);
		}
		if (a_state.door == a_door->GetFormID() && a_state.hidden == a_hide && a_state.passable == a_passable && a_state.solid == a_solid) {
			return;
		}
		Close(a_state);
		a_state.door = a_door->GetFormID();
		a_state.solid = a_solid;
		auto& shared = openDoors[a_state.door];
		if (shared.opens++ == 0) {
			shared.root.reset(door3D);
			CullBehindPanel(a_door, shared);
		}
		if (!a_solid && shared.walkables++ == 0) {
			// The doorway can be walked through. Using the door is the manager's (DoorActivateHook):
			// shut it, or nothing, never the ordinary transition with its loading screen.
			MakeWalkable(a_door, door3D, shared);
		}
		if (a_hide) {
			if (shared.hides++ == 0) {
				HideShapes(a_door, shared);
			}
			a_state.hidden = true;
		}
		if (a_passable) {
			if (shared.passables++ == 0) {
				HideBodies(door3D, shared);
			}
			a_state.passable = true;
		}
		logger::info("portal: door {:08X} open ({} collision objects, {} shape(s) behind the panel off, {} opener(s){}{})", a_state.door, shared.layers.size(), shared.behindShapes.size(), shared.opens, a_hide ? ", hidden" : "",
			a_passable ? std::format(", passable: {} bodies out", shared.removedBodies.size()) : "");
	}

	void Close(OpenState& a_state)
	{
		const auto it = a_state.door ? openDoors.find(a_state.door) : openDoors.end();
		if (it != openDoors.end()) {
			auto& shared = it->second;
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_state.door);
			if (a_state.hidden && --shared.hides == 0) {
				for (auto& shape : shared.hiddenShapes) {
					Pristine::Show(shape.get());
				}
				shared.hiddenShapes.clear();
			}
			if (a_state.passable && --shared.passables == 0) {
				ShowBodies(door, shared);
			}
			if (!a_state.solid && --shared.walkables == 0) {
				for (auto& [object, layer] : shared.layers) {
					object->SetCollisionLayer(layer);
					Pristine::LayerBack(object.get());
				}
				shared.layers.clear();
			}
			if (--shared.opens == 0) {
				for (auto& shape : shared.behindShapes) {
					Pristine::Show(shape.get());
				}
				openDoors.erase(it);
			}
		}
		a_state = {};
	}
}