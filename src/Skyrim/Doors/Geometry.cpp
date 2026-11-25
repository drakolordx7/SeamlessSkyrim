#include "Skyrim/Doors/Geometry.h"

#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Portal/Pristine.h"

namespace Portal::Doors
{
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

	// The cell's own coordinates to where it is drawn: Placement undone.
	RE::NiTransform ToShown(RE::TESObjectREFR* a_door)
	{
		auto* door3D = a_door->Get3D();
		auto* cell3D = door3D ? CellNodeAbove(a_door, door3D) : nullptr;
		return cell3D ? cell3D->world : RE::NiTransform{};
	}

	void TurnLeaves(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, float a_amount, RE::FormID a_settings)
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

	void RestoreLeaves(RE::FormID a_door)
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

	bool IsSwung(RE::FormID a_door)
	{
		std::scoped_lock lock(geometryLock);
		return swungClosed.contains(a_door);
	}

	void ForgetUnloaded()
	{
		std::scoped_lock lock(geometryLock);
		std::erase_if(geometries, [](const auto& a_entry) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_entry.first);
			return !door || door->Get3D() != a_entry.second.root.get();
		});
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

	RE::NiPoint3 PanelNormal(RE::TESObjectREFR* a_door)
	{
		const auto corners = Corners(a_door);
		bool       thinX;
		{
			std::scoped_lock lock(geometryLock);
			thinX = GeometryOf(a_door).thinX;
		}
		return corners[thinX ? 1 : 2] - corners[0];
	}

	std::vector<RE::NiPointer<RE::NiAVObject>> LeafNodes(RE::TESObjectREFR* a_door)
	{
		std::scoped_lock                           lock(geometryLock);
		std::vector<RE::NiPointer<RE::NiAVObject>> nodes;
		for (const auto& leaf : GeometryOf(a_door).leaves) {
			nodes.push_back(leaf.node);
		}
		return nodes;
	}

	std::array<RE::NiPoint3, 4> Quad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, QuadOptions a_options)
	{
		bool thinX;
		Box  box;
		{
			std::scoped_lock lock(geometryLock);
			const auto& geometry = GeometryOf(a_door);
			thinX = geometry.thinX;
			box = geometry.panel;
			if (a_options.wholeMesh) {
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
		const bool maxFace = maxSide != a_options.behindPanel;
		const int  face = maxFace ? thinBit : 0;
		const auto shift = outward * (maxFace ? 2.0f : -2.0f);
		return { c[face] + shift, c[face | wideBit] + shift, c[face | 4] + shift, c[face | wideBit | 4] + shift };
	}

	std::array<RE::NiPoint3, 4> ShownQuad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, QuadOptions a_options)
	{
		const auto toShown = ToShown(a_door);
		auto       quad = Quad(a_door, toShown.Invert() * a_viewer, a_options);
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

	std::vector<RE::NiPointer<RE::BSGeometry>> BehindPanel(RE::TESObjectREFR* a_door)
	{
		auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		auto  partner = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		auto* partnerTeleport = partner ? partner->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
		if (!partnerTeleport || !partnerTeleport->teleportData) {
			return {};
		}
		const auto       front = DoorFrame(a_door).Invert() * partnerTeleport->teleportData->position;
		std::scoped_lock lock(geometryLock);
		const auto&      geometry = GeometryOf(a_door);
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
