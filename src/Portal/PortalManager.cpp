#include "Portal/PortalManager.h"

#include "Diag/Telemetry.h"
#include "Engine/Fade.h"
#include "Engine/Hooks.h"
#include "Engine/Paths.h"
#include "Engine/SameSpace.h"

#include "Portal/CameraLimit.h"
#include "Portal/Pristine.h"
#include "Doors/DoorUse.h"
#include "Doors/Geometry.h"
#include "Doors/Hold.h"
#include "Doors/Host.h"
#include "Doors/Pair.h"
#include "Doors/Policy.h"
#include "Doors/Settings.h"
#include "Doors/Swing.h"
#include "Pictures/DoorwaySnapshot.h"
#include "Portal/LightOcclusion.h"
#include "Portal/LightScene.h"
#include "Portal/MergedCull.h"
#include "Portal/NpcTravel.h"
#include "Render/Frame.h"
#include "Render/SceneMeter.h"
#include "Render/SceneQuery.h"
#include "Render/SpaceLighting.h"
#include "Portal/Travel.h"
#include "Spaces/RemoteCell.h"
#include "Spaces/Tuning.h"
#include "Plugin/Settings.h"
#include "Portal/ShapeSet.h"
#include "Portal/StreetLOD.h"
#include "Portal/WaterReflections.h"
#include "Diag/Probe.h"
#include "Render/Backend.h"
#include "Render/DoorwayPlanes.h"

#include <Windows.h>

namespace
{
	// Walking into a merged room (from outdoors) without the game switching into it at the door: the
	// room's collision is copied into the world (RemoteCell::MergeCollision), and the game switches
	// into the room only where the doorway can't be seen (see Approach). Seen from inside, the
	// doorway shows the game's own outdoors (its shadows, distant land and sky), not a picture drawn
	// by this plugin. [Portals] bWalkIntoRooms, off by default: every switch in the middle of a room
	// showed (a long frame, the light stepping, the player's own head for a frame), it happened far
	// more often than walking through doors does (58 times in seven minutes of play), and the room's
	// people, in another cell than the player, can't see, greet or fight them meanwhile.
	bool            walkIntoRooms = false;
	constexpr float kMinRoomDepth = 120.0f;        // the switch into the room never happens closer to the door
	constexpr float kRoomSwitchOutDepth = 180.0f;  // walking towards the door, outdoors from here on whatever the view
	// ... and from this near on with the doorway in view. Not from anywhere it could be seen: every
	// switch is a visible step (a long frame, the light changing), and switched out from across the
	// room (the doorway seen 300-550 units away), the player was switched straight back in by the
	// rules below (upstairs, far from the door, the doorway turned away from): 58 switches in seven
	// minutes of play, the Hall of the Dead's stairs once a second.
	constexpr float kRoomSwitchOutViewDepth = 350.0f;
	constexpr float kApproachSeconds = 0.25f;  // walking towards the door this long counts as heading out
	float           approachingFor = 0.0f;
	constexpr float kUpstairsHeight = 60.0f;       // feet this far above the doorway's floor: in the room itself (see Approach)
	// Doorway "in view": its corners within the screen widened by this much (a fraction of it).
	// Switching out needs it seen with the smaller margin, switching in unseen with the larger one,
	// so turning at the edge doesn't switch back and forth.
	constexpr float kSeenMargin = 0.15f;
	constexpr float kUnseenMargin = 0.35f;
	constexpr float kUnseenSeconds = 3.0f;    // the doorway unseen this long before the switch into the room
	float           doorwayUnseenFor = 0.0f;
	bool            playerInRoom = false;  // last frame: walking in a room walked into (see Approach)

	// A save made while walking in such a room (Manager::NoteRoomForSave): the outdoor door and where
	// the player is, in the room's own space.
	struct RoomSave
	{
		RE::FormID   door = 0;
		RE::NiPoint3 position;
		float        yaw = 0.0f;
	};
	std::optional<RoomSave> roomToSave;  // for the save being written
	std::optional<RoomSave> roomLoaded;  // from the save just loaded
	constexpr std::uint32_t kRoomRecord = 'ROOM';
	constexpr std::uint32_t kRoomRecordVersion = 1;

	// Whether the doorway (its opening, at the door's plane) is on screen or close to it. Walls in
	// between aren't considered: seen through one, the switch outdoors happens a bit early.
	bool DoorwayInView(const Portal::Doors::Mapping& a_mapping, float a_margin)
	{
		auto* camera = RE::Main::WorldRootCamera();
		if (!camera) {
			return true;
		}
		const RE::NiPoint3 right{ a_mapping.inward.y, -a_mapping.inward.x, 0.0f };
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
		float              minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
		int                behind = 0;
		for (const float sx : { -1.0f, 1.0f }) {
			for (const float sz : { -1.0f, 1.0f }) {
				const auto corner = a_mapping.sourceCenter + right * (sx * a_mapping.halfWidth) + up * (sz * a_mapping.halfHeight);
				float      x = 0.0f, y = 0.0f, z = 0.0f;
				camera->WorldPtToScreenPt3(corner, x, y, z, 1e-5f);
				if (z < 0.0f) {
					++behind;
					continue;
				}
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
			}
		}
		if (behind == 4) {
			return false;
		}
		if (behind > 0) {
			return true;  // partly behind the camera: the doorway is right at it
		}
		return maxX >= -a_margin && minX <= 1.0f + a_margin && maxY >= -a_margin && minY <= 1.0f + a_margin;
	}

	// The reach of a portal's door: the space behind it loads kLoadAhead further out ([Portals]
	// iDoorOpenDistance; it was the distance doors swung open at, before they were opened by use).
	float           doorOpenRadius = static_cast<float>(Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance).def);
	// Few doors around the player (the scan, NearestPortalDoor): nothing competes for what is loaded,
	// so it is loaded from much further out without spaces being swapped as the player moves.
	// Outdoors the kept rooms reach further (warmReach); indoors, where one space is loaded, and
	// outdoors near a door into another worldspace, the shown one does (farActivate, 0 when it
	// doesn't apply).
	bool            sparse = false;
	float           farActivate = 0.0f;
	float           NearRadius() { return doorOpenRadius + Portal::Manager::kLoadAhead; }
	float           ActivateRadius() { return std::max(NearRadius(), farActivate); }
	float           ReleaseRadius() { return ActivateRadius() + Portal::Manager::kReleaseBeyond; }
	// A different door must be this much closer before we switch portals.
	constexpr float kSwitchMargin = 200.0f;
	// How long after a space behind a door is attached its references skip the engine's fade-in
	// (merged, SkipFadeIn takes over): long enough for those loaded last.
	constexpr float kShowAtOnceSeconds = 3.0f;
	// Walking "into" the door: horizontal distance to the door's centre while facing it.
	constexpr float kCrossDistance = 90.0f;
	constexpr float kRetryCooldown = 5.0f;

	bool                                   enabled = false;
	bool                                   hooksInstalled = false;  // see Install
	// Render-merge: from outside, the interior behind the door is hung into the world scene and
	// drawn by the main pass (the patched Community Shaders masks the two spaces against each
	// other) instead of the separate portal pass.
	bool                                   mergeEnabled = true;
	// Prototype (docs/APPROACHES.md, 9): the street merged into the interior the player is in, so the
	// view out is drawn by the game's main pass instead of the portal pass. [Portals] bMergeOutdoors.
	bool                                   mergeOutdoors = true;
	Portal::RemoteCell                     remote;  // the space shown behind the portal's door
	// Interiors behind other doors near the player, attached out of sight, so that their doorway
	// opens at once when it becomes the nearest instead of waiting for the space to load. Outdoors
	// only: from indoors the space behind a door is a street, and two streets would share cells.
	// Those whose door is open are also merged beside the shown room (besideRooms).
	std::vector<Portal::RemoteCell> warm;
	// How near their door must be. Let go only kWarmMargin further out, so
	// walking along that boundary doesn't attach and let go of the same room over and over.
	constexpr float                 kWarmDistance = 2000.0f;
	constexpr float                 kWarmMargin = 400.0f;
	// With few doors around (sparse), rooms are kept from this many times as far: warmReach.
	constexpr float                 kSparseReach = 2.0f;
	float                           warmReach = kWarmDistance;
	// Up to this many spaces behind doors within reach (or as many as are loaded at once, if more)
	// count as few.
	constexpr std::size_t           kFewDoors = 3;
	// One attach at a time, a few times a second at most: each makes a long frame (3-10 ms in
	// Whiterun's market, more for a room whose 3D has to be loaded).
	constexpr float                 kWarmAttachSeconds = 0.5f;
	float                           warmAttachTimer = 0.0f;
	// [Portals] iShownRooms: outdoors, how many rooms behind doors are active at once, the shown one
	// (the nearest door's) included: kept loaded (warm), and those whose door is open drawn in their
	// doorway beside it, as many as the render backend can mask (Render::Backend::MergedRooms).
	// Kept loaded and drawn were two settings; a room kept loaded but drawn as a picture was only
	// ever a wait cut short.
	int                             shownRoomsSetting = Plugin::Settings::Of(Plugin::Settings::Id::kShownRooms).def;
	// [Portals] bPauseUnseenRooms: a kept room nobody can see (kept out of sight, or merged beside
	// the shown one with its doorway off screen) is paused: not stepped, lights not updated, not
	// drawn, its people not on screen. Facing away, each kept room cost about 1 ms a frame.
	bool                            pauseUnseen = true;
	// Out of sight this long before a room merged beside the shown one pauses, so that looking
	// around past its doorway doesn't pause and wake it every few frames. Waking is at once.
	constexpr float                 kPauseAfterSeconds = 0.5f;

	// The shapes of a merged space, listed anew now and then (NPCs change clothes, things get
	// dropped), a few roots per frame: done whole in one frame it was ~3 ms twice a second with
	// a street merged in, felt as judder. Each root's bounds the engine fixed since the merge are
	// unfixed on the way (see MovedBounds::Unfix).
	struct MarkPass
	{
		struct Root
		{
			RE::NiPointer<RE::NiAVObject> node;
			const RE::TESObjectCELL*      cell = nullptr;  // a merged cell's 3D; null: the distant land's
		};
		std::vector<Root>                          roots;
		std::size_t                                next = 0;
		Portal::ShapeSet                           shapes;
		std::vector<RE::NiPointer<RE::BSGeometry>> strays;
		RE::NiPoint3                               doorway;  // stray bones are measured from it
	};

	// What a room merged into the world needs while it is drawn there, besides its cells
	// (RemoteCell): its doors, its listed shapes, its light, its people's 3D. Kept apart from the
	// cells so that a room handed from being shown beside the nearest door to being the shown one
	// (or back) is drawn on as it was: nothing swung shut, listed again or unlit for a frame.
	struct MergedRoom
	{
		bool                    marked = false;  // listed at least once in this merge (MarkMergedShapes)
		float                   markTimer = 0.0f;
		std::optional<MarkPass> markPass;
		Portal::ShapeSet        shapes;  // main thread only (see roomsBySlot)
		// Skinned shapes hidden for their stray bones (see HasStrayBones).
		std::vector<RE::NiPointer<RE::BSGeometry>> strays;
		// Shader properties whose "casts shadows" was taken off while merged (FinishMarkPass).
		std::vector<RE::NiPointer<RE::BSShaderProperty>> unshadowed;
		float                                lightingTimer = 0.0f;
		std::optional<Render::SpaceLight>    lighting;
		RE::FormID                           door = 0;         // the door in the player's space it is merged behind
		float                                daylight = 0.0f;  // as sent (Render::Backend::SetMergedDaylight)
		// Actor 3D moved out of its cell's actor node while merged, with the node to put it back in.
		std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::NiPointer<RE::NiNode>>> actors;
		float                                seconds = 0.0f;  // since the merge began
		Portal::Doors::OpenState             sourceDoor;
		Portal::Doors::OpenState             linkedDoor;
		Portal::Doors::Swing                 swing;  // of the swung door (swungDoor)
		RE::FormID                           swungDoor = 0;  // which of the pair that is (Doors::SwingingDoor)
		std::vector<Portal::Doors::OpenState> extraDoors;
		bool                                 extraScanned = false;
	};
	std::unique_ptr<MergedRoom> primaryRoom = std::make_unique<MergedRoom>();  // remote's, while merged

	// Where a space merged through a_portal stands.
	Portal::RemoteCell::Placement PlacementOf(const Portal::Manager::Active& a_portal, const Portal::Doors::Mapping& a_mapping)
	{
		using RE::TESForm;
		auto*      source = TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		auto*      linked = TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor);
		const auto key = Portal::Doors::CorrectionDoor(source, linked)->GetFormID();
		auto       toSource = a_mapping.toLinked.Invert();
		// A pair's two doors can be the same door placed at different sizes: Understone Keep's is 0.9
		// on the street, 1.0 inside. Seen from inside, the street's doorway was smaller than the
		// keep's, and through the keep's the street's facade around it showed, where the keep's own
		// door frame stands. The street seen from inside is drawn larger by their ratio, around its
		// door, so that the two doorways are one. Far off it looks the same (as much larger as it is
		// further away); close to the doorway it shrinks back on walking out. Only the street: a room
		// seen from outside is walked into on its collision, which can't be scaled.
		if (!a_portal.cell->IsInteriorCell() && source->GetBaseObject() == linked->GetBaseObject()) {
			const float ratio = Portal::Doors::Size(source).halfHeight / Portal::Doors::Size(linked).halfHeight;
			if (std::abs(ratio - 1.0f) > 0.01f) {
				const auto      center = Portal::Doors::Center(linked);
				RE::NiTransform grow;
				grow.scale = ratio;
				grow.translate = center - center * ratio;
				toSource = toSource * grow;
			}
		}
		return { toSource, a_portal.sourceDoor, Portal::Doors::Correction(key), Portal::Doors::Shift(key) };
	}

	// A kept room (in warm, by its cell) merged beside the shown one, through its nearest door.
	struct BesideRoom
	{
		RE::TESObjectCELL*          cell = nullptr;
		RE::FormID                  sourceDoor = 0;
		RE::FormID                  linkedDoor = 0;
		int                         slot = 0;  // with the render backend, 1..3
		std::unique_ptr<MergedRoom> state;
		bool                        seen = true;     // its doorway in the last player view (BeforePlayerView)
		float                       unseen = 0.0f;   // seconds since it was
		bool                        paused = false;  // see pauseUnseen; its cells parked meanwhile
	};
	std::vector<BesideRoom> besideRooms;

	bool BesideDoor(RE::FormID a_door)
	{
		return std::ranges::any_of(besideRooms, [&](const BesideRoom& a_room) { return a_room.sourceDoor == a_door; });
	}

	// A kept space let go, shown beside the nearest door or not.
	std::vector<Portal::RemoteCell>::iterator LetGoKept(std::vector<Portal::RemoteCell>::iterator a_space, std::string_view a_why);
	std::mutex                             activeLock;
	std::optional<Portal::Manager::Active> active;
	RE::FormID                             failedCell = 0;
	float                                  failedCooldown = 0.0f;
	float                                  crossCooldown = 0.0f;
	// After handing a cell to the engine's door transition, don't grab it back while the
	// transition is still pending (the next frame re-attached it right before the engine took it).
	float                                  attachHoldoff = 0.0f;
	std::string                            lastEvent = "none";
	std::optional<float>                   approachLimit;  // camera pull-in this frame, see Approach

	struct Candidate
	{
		RE::TESObjectREFR* door;
		RE::FormID         linkedDoor;
		RE::TESObjectCELL* cell;
		float              distance;
		float              rank;  // see Rank
	};

	// The direction the player last moved in (horizontal, unit; zero until they have, in this
	// space): the door walked towards is the likelier one to be walked through.
	RE::NiPoint3    heading;
	RE::NiPoint3    headingFrom;
	// Half the switch margin: turning round between two doors equally far changes their order by
	// twice this, which by itself must not swap what is loaded.
	constexpr float kHeadingNudge = kSwitchMargin / 2.0f;

	void TrackHeading(const RE::NiPoint3& a_position)
	{
		auto step = a_position - headingFrom;
		step.z = 0.0f;
		const float length = step.Length();
		// Over a few frames' worth of walking: a frame's step is too short to give a direction.
		if (length < 32.0f) {
			return;
		}
		headingFrom = a_position;
		// A jump is a teleport or a crossing into another space's coordinates, no direction.
		heading = length > 500.0f ? RE::NiPoint3() : step / length;
	}

	// A city gate (outdoors, into another worldspace) is many times a house door's size and seen
	// from far down the road: it counts as nearer than it is, in which door is shown and in how far
	// out its space loads and is let go. Not by a fixed amount: a house door must still win when
	// walked up to near a gate (it has to be the shown one to be walked through), and at a quarter
	// of the distance it does unless it stands within four times the switch margin of the gate
	// (kSwitchMargin / kGateWeight).
	constexpr float kGateWeight = 0.25f;

	// a_door's distance from a_origin as the reaches and the order of doors count it.
	float Reach(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		const float distance = a_origin.GetDistance(a_door->GetPosition());
		return Portal::Doors::Outdoors(a_door) && Portal::Doors::Outdoors(a_linked) ? distance * kGateWeight : distance;
	}

	// What doors are ordered by: the reach, a little less for a door ahead of the player and a
	// little more for one behind.
	float Rank(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		const auto to = a_door->GetPosition() - a_origin;
		return Reach(a_origin, a_door, a_linked) - kHeadingNudge * heading.Dot(to) / std::max(to.Length(), 1.0f);
	}

	// Load doors into another space: outside -> interior, interior -> outside or another interior,
	// and from one worldspace into another (a walled city's gate). Exterior doors within one
	// worldspace aren't portals: both ends are in the scene already.
	// The door at the other end of a_ref when a_ref would be a portal door, the menu's switch aside.
	RE::NiPointer<RE::TESObjectREFR> PortalLink(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_here)
	{
		auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
		// A locked door stays a door: closed, solid, to be picked or opened with its key. Unless
		// every door is to stand open (DoorUse's force).
		const bool locked = a_ref && a_ref->IsLocked() && Portal::DoorUse::GetForce() != Portal::DoorUse::Force::kAllOpen;
		if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDisabled() || !a_ref->Get3D() || locked) {
			return nullptr;
		}
		// A marker door is no doorway: unseen, used by scripts (Riften's prison marker, which takes
		// an arrested player to the jail). Its portal stood in the open street, next to the keep.
		if (static_cast<RE::TESObject*>(base)->IsMarker()) {
			return nullptr;
		}
		auto* teleport = a_ref->extraList.GetByType<RE::ExtraTeleport>();
		auto  linked = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		if (!linked) {
			return nullptr;
		}
		// A persistent exterior door whose cell isn't loaded (a game loaded indoors) has no
		// parent cell yet; it stands outdoors in its worldspace.
		auto*       cell = linked->GetParentCell();
		auto* const linkedWorld = cell ? (cell->IsInteriorCell() ? nullptr : cell->GetRuntimeData().worldSpace) : linked->GetWorldspace();
		const bool  sameWorld = linkedWorld && !a_here->IsInteriorCell() && linkedWorld == a_here->GetRuntimeData().worldSpace;
		if ((!cell && !linkedWorld) || cell == a_here || sameWorld) {
			return nullptr;
		}
		return linked;
	}

	// The interiors behind portal doors within reach of the last scan, best ranked first, each once
	// (several doors can lead into one inn). Only collected outdoors, while spaces are kept warm.
	struct WarmTarget
	{
		RE::FormID cell;
		float      distance;
		float      rank;    // see Rank
		RE::FormID door;    // the nearest door into it, in the player's space
		RE::FormID linked;  // its other end, in the cell
	};
	std::vector<WarmTarget> warmTargets;

	// The portal door to show, within ActivateRadius; also sets warmTargets, sparse and the reaches
	// that follow from it.
	std::optional<Candidate> NearestPortalDoor(RE::PlayerCharacter* a_player)
	{
		warmTargets.clear();
		struct Found
		{
			RE::TESObjectREFR* door;
			RE::NiPointer<RE::TESObjectREFR> linked;
			RE::TESObjectCELL* cell;  // null: an exterior cell not loaded yet
			float              distance;
			float              reach;  // see Reach
			float              rank;
		};
		std::vector<Found> found;
		auto*              here = a_player->GetParentCell();
		const auto         origin = a_player->GetPosition();
		const bool         outdoors = !here->IsInteriorCell();
		const bool         collectWarm = shownRoomsSetting > 1 && outdoors;
		const float        farReach = outdoors ? kWarmDistance * kSparseReach : kWarmDistance;
		RE::TES::GetSingleton()->ForEachReferenceInRange(a_player, farReach + kWarmMargin, [&](RE::TESObjectREFR* a_ref) {
			auto linked = PortalLink(a_ref, here);
			if (!linked) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			// Switched off in the menu: an ordinary door (stored under either of the pair).
			if (Portal::Doors::PortalOff(a_ref->GetFormID()) || Portal::Doors::PortalOff(linked->GetFormID())) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			found.push_back({ a_ref, linked, linked->GetParentCell(), origin.GetDistance(a_ref->GetPosition()), Reach(origin, a_ref, linked.get()), Rank(origin, a_ref, linked.get()) });
			return RE::BSContainer::ForEachResult::kContinue;
		});
		{
			// Spaces, not doors: an inn's two doors load one room.
			const auto spaces = [&](float a_within) {
				std::vector<RE::FormID> ids;
				for (const auto& door : found) {
					const auto id = door.cell ? door.cell->GetFormID() : door.linked->GetFormID();
					if (door.distance <= a_within && std::ranges::find(ids, id) == ids.end()) {
						ids.push_back(id);
					}
				}
				return ids.size();
			};
			// Between the reach and the scan's margin beyond it, as it was: a door at the edge
			// doesn't change the reach, and with it what is loaded, back and forth.
			const auto few = std::max(kFewDoors, static_cast<std::size_t>(outdoors ? shownRoomsSetting : 1));
			if (spaces(farReach) > few) {
				sparse = false;
			} else if (spaces(FLT_MAX) <= few) {
				sparse = true;
			}
			// Outdoors a kept room stands in for loading the shown one early, but only interiors are
			// kept: a city gate's worldspace was loaded at the near reach, its low-detail stand-in seen
			// through the open gate until then.
			const bool worldDoor = std::ranges::any_of(found, [&](const Found& a_door) {
				return a_door.distance <= farReach && (!a_door.cell || !a_door.cell->IsInteriorCell());
			});
			farActivate = sparse && (!outdoors || worldDoor) ? farReach : 0.0f;
			warmReach = sparse && outdoors ? farReach : kWarmDistance;
		}
		const Found* best = nullptr;
		for (const auto& door : found) {
			if (collectWarm && door.cell && door.cell->IsInteriorCell()) {
				// A room with two doors: which room is kept goes by the better rank of the two, but
				// the door it is shown through is the nearer one. Taken by rank as well, the door
				// changed as the player turned on the spot, to one out of reach, and the room's open
				// doorway in front of them went shut.
				const auto room = std::ranges::find(warmTargets, door.cell->GetFormID(), &WarmTarget::cell);
				if (room == warmTargets.end()) {
					warmTargets.push_back({ door.cell->GetFormID(), door.distance, door.rank, door.door->GetFormID(), door.linked->GetFormID() });
				} else {
					room->rank = std::min(room->rank, door.rank);
					if (door.distance < room->distance) {
						room->distance = door.distance;
						room->door = door.door->GetFormID();
						room->linked = door.linked->GetFormID();
					}
				}
			}
			if (door.reach <= ActivateRadius() && (!best || door.rank < best->rank)) {
				best = &door;
			}
		}
		std::ranges::sort(warmTargets, {}, &WarmTarget::rank);
		if (!best) {
			return std::nullopt;
		}
		auto* cell = best->cell;
		if (!cell) {
			// Loaded for the nearest door only: done for every door in range, every scan, it could
			// go to disk for cells nobody was walking towards.
			auto* world = best->linked->GetWorldspace();
			SEAMLESS_ASSERT(world);  // chosen above only with a worldspace when it has no cell
			const auto pos = best->linked->GetPosition();
			cell = world->LoadCell(static_cast<std::int16_t>(std::floor(pos.x / 4096.0f)), static_cast<std::int16_t>(std::floor(pos.y / 4096.0f)));
			if (!cell) {
				return std::nullopt;
			}
		}
		return Candidate{ best->door, best->linked->GetFormID(), cell, best->distance, best->rank };
	}

	// The scan above walks every reference within ActivateRadius; doors don't come and go that
	// fast, so it runs a few times a second and the result is looked up again in between.
	constexpr float kScanSeconds = 0.2f;
	float           scanTimer = 0.0f;
	struct ScannedDoor
	{
		RE::FormID door;
		RE::FormID linkedDoor;
		RE::FormID cell;
	};
	std::optional<ScannedDoor> scanned;

	std::optional<Candidate> CandidateDoor(RE::PlayerCharacter* a_player, float a_delta)
	{
		if ((scanTimer -= a_delta) <= 0.0f) {
			scanTimer = kScanSeconds;
			const auto found = NearestPortalDoor(a_player);
			scanned = found ? std::optional(ScannedDoor{ found->door->GetFormID(), found->linkedDoor, found->cell->GetFormID() }) : std::nullopt;
			return found;
		}
		if (!scanned) {
			return std::nullopt;
		}
		// By ID: a door is a temporary reference of its cell and goes with it.
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(scanned->door);
		auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(scanned->cell);
		if (!door || !door->Get3D() || !cell) {
			return std::nullopt;
		}
		auto*      linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(scanned->linkedDoor);
		const auto origin = a_player->GetPosition();
		// A teleport between scans leaves the last scan's door far behind: attached, it was let go
		// at once as walked away from, and attached again, every frame until the next scan.
		if (Reach(origin, door, linked) > ActivateRadius()) {
			return std::nullopt;
		}
		return Candidate{ door, scanned->linkedDoor, cell, origin.GetDistance(door->GetPosition()), Rank(origin, door, linked) };
	}

	RE::TESObjectREFR* Lookup(RE::FormID a_id)
	{
		return RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
	}

	void SetActive(std::optional<Portal::Manager::Active> a_active)
	{
		std::scoped_lock lock(activeLock);
		active = a_active;
	}

	std::optional<Portal::Manager::Active> GetActive()
	{
		std::scoped_lock lock(activeLock);
		return active;
	}

	// The source door stands open while its portal is shown: the player can walk into the doorway.
	Portal::Doors::OpenState openDoor;
	// A city gate's far gate (OpenDoor): the cells of both worldspaces share one Havok world, and
	// the other worldspace's gate, closed, stands a few units behind the player's own.
	Portal::Doors::OpenState openFarGate;
	Portal::Doors::Swing     openSwing;  // how far the panel has swung open

	// Far enough past ReleaseRadius (fast travel on horseback), a door still swinging is let go
	// anyway. Past it, by reach (Reach), as ReleaseRadius is: a fixed distance stood inside a gate's
	// release reach and a sparse street's, and those were let go at once, swinging or not.
	constexpr float kForceReleaseBeyond = 1350.0f;
	bool            doorWanted = false;  // the portal's door swings towards open (else towards shut): Wanted

	// The space behind the door is drawn in its doorway (merged, the portal ready, or a room beside),
	// not a stand-in's picture.
	bool SpaceShown(RE::FormID a_door)
	{
		const auto portal = GetActive();
		return (portal && portal->sourceDoor == a_door && (remote.Merged() || Portal::Manager::ReadyPortal())) || BesideDoor(a_door);
	}

	RE::FormID openDoorSwung = 0;  // the door OpenDoor swings: the source, or the linked door seen through it

	void TakeOverDoor(Portal::Doors::Swing& a_swing, RE::FormID a_door);

	void OpenDoor(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_inward, float a_delta)
	{
		if (!a_door) {
			return;
		}
		// A door swings open into the space it opens into (Doors::SwingingDoor): the one the player
		// is in, off the portal image on its face (with the panel left in place, the two fought over
		// the same pixels), or the space behind, the linked door swung in the portal's picture and
		// this one hidden. A city gate too; its leaves were hidden once, and the gateway stood
		// empty.
		auto*      here = a_door->GetParentCell();
		const auto portal = GetActive();
		const bool gate = here && !here->IsInteriorCell() && portal && portal->cell && !portal->cell->IsInteriorCell();
		auto*      linked = portal ? Lookup(portal->linkedDoor) : nullptr;
		const bool farSwings = linked && Portal::Doors::SwingingDoor(a_door, linked) == linked->GetFormID();
		auto*      swungDoor = farSwings ? linked : a_door;
		const auto swung = swungDoor->GetFormID();
		if (openDoor.door != a_door->GetFormID() || openDoorSwung != swung) {
			Portal::Doors::RestoreLeaves(openDoor.door);
			Portal::Doors::RestoreLeaves(openDoorSwung);
			TakeOverDoor(openSwing, swung);
			openDoorSwung = swung;
		}
		Portal::Doors::Swing::Reclaim(swung);
		// Timed by the door that swings; the space behind, holding it, may still be loading.
		auto* timed = swungDoor->Get3D() ? swungDoor : a_door;
		// Only from shut: the picture that makes a door open from afar is taken while it swings open
		// the first time, and snapped open then it jumped.
		const bool wasShut = openSwing.Step(timed, doorWanted, a_delta);
		if (doorWanted && wasShut && linked && Portal::Doors::OpenFromAfar(a_door, linked)) {
			openSwing.SnapOpen();
		}
		if (openSwing.Shut()) {
			// Swung shut: an ordinary door again, solid, a gate shown. The portal isn't drawn on
			// it any more (DoorwayOpen); its quad lies on the panel's face.
			Portal::Doors::RestoreLeaves(a_door->GetFormID());
			Portal::Doors::RestoreLeaves(openDoorSwung);
			Portal::Doors::Close(openDoor);
			Portal::Doors::Close(openFarGate);
			remote.RestorePassage();
			return;
		}
		const auto settings = linked ? Portal::Doors::CorrectionDoor(a_door, linked)->GetFormID() : a_door->GetFormID();
		const bool hide = farSwings;
		if (openDoor.door == a_door->GetFormID() && openDoor.hidden != hide) {
			Portal::Doors::RestoreLeaves(openDoor.door);
		}
		Portal::Doors::Open(a_door, openDoor, { .hide = hide, .passable = gate });
		// Walking out of Whiterun the player stood on its housing, 25 units short of the doorway,
		// once the view out was there, and wasn't carried through. The far gate is drawn by the
		// portal pass: swung open when it is the one that swings, else hidden.
		if (gate && linked) {
			Portal::Doors::Open(linked, openFarGate, { .hide = !farSwings, .passable = true });
			if (const auto mapping = Portal::Doors::TryCompute(*portal)) {
				remote.ClearPassage(mapping->sourceCenter, mapping->inward, mapping->halfWidth, mapping->halfHeight);
			}
		} else {
			Portal::Doors::Close(openFarGate);
			remote.RestorePassage();
		}
		if (farSwings) {
			// Into the space behind, in its own coordinates (the portal pass draws it where it is).
			if (const auto mapping = Portal::Doors::TryCompute(*portal)) {
				openSwing.Turn(timed, linked, mapping->toLinked.rotate * mapping->inward, settings);
			}
		} else if (!hide) {
			openSwing.Turn(timed, a_door, RE::NiPoint3{ -a_inward.x, -a_inward.y, 0.0f }, settings);
		} else {
			Portal::Doors::RestoreLeaves(a_door->GetFormID());
		}
	}

	void CloseDoor()
	{
		if (openDoor.door) {
			openSwing.LetGo(openDoorSwung ? openDoorSwung : openDoor.door);
		}
		openSwing.Reset();
		openDoorSwung = 0;
		Portal::Doors::Close(openDoor);
		Portal::Doors::Close(openFarGate);
		remote.RestorePassage();
	}

	bool MergeSupported()
	{
		return mergeEnabled && ::Render::Backend::CanMerge();
	}

	// Merging puts an interior into the exterior's scene. The other way round (the street, its land
	// and the distant LOD put into an interior's scene) is the mergeOutdoors prototype: indoors the
	// LOD stays loaded, only hidden, and the sky's dome stays in the scene (bridge outdoors_probe).
	bool Mergeable(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behindDoor)
	{
		if (!MergeSupported() || !a_here || !a_behindDoor) {
			return false;
		}
		return (!a_here->IsInteriorCell() && a_behindDoor->IsInteriorCell()) || (mergeOutdoors && a_here->IsInteriorCell() && !a_behindDoor->IsInteriorCell());
	}

	// The street merged into the interior the player is in (mergeOutdoors), as opposed to a room
	// merged into the street.
	bool MergedOutdoors()
	{
		return remote.Merged() && remote.Cell() && !remote.Cell()->IsInteriorCell();
	}

	// A door into a room has its doorway picture taken with a camera of its own
	// (Portal::Snapshot::CaptureCamera), which only the portal pass can draw with: merged, the room
	// is drawn with the world, and it is merged as soon as it has loaded. Once per door (the
	// picture is kept), the merge waits for it: a moment for the room's late lights, then the
	// frames it takes. Merged anyway if it doesn't come.
	struct PictureWait
	{
		RE::FormID                                           door = 0;
		std::optional<std::chrono::steady_clock::time_point> since;
		bool                                                 gaveUp = false;
	};
	PictureWait    pictureWait;
	RE::FormID     pictureWanted = 0;  // of the render, this frame
	constexpr auto kPictureLightsSettle = std::chrono::milliseconds(300);
	// A big room takes a few dozen capture frames to measure its lights for (LightOcclusion).
	constexpr auto kPictureGiveUp = std::chrono::milliseconds(3000);

	// a_mergeable: the room would be merged now.
	bool WaitForPicture(const Portal::Manager::Active& a_portal, const RE::TESObjectCELL* a_here, bool a_mergeable)
	{
		pictureWanted = 0;
		if (pictureWait.door != a_portal.sourceDoor) {
			pictureWait = { a_portal.sourceDoor };
		}
		if (!a_mergeable || a_here->IsInteriorCell() || !a_portal.cell->IsInteriorCell()) {
			pictureWait.since.reset();
			return false;
		}
		auto* door = Lookup(a_portal.sourceDoor);
		if (pictureWait.gaveUp ||
			!Portal::Snapshot::CameraPictureWanted(a_portal.sourceDoor, Portal::Doors::Quad(door, RE::PlayerCharacter::GetSingleton()->GetPosition(), { .behindPanel = true }))) {
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!pictureWait.since) {
			pictureWait.since = now;
		}
		if (now - *pictureWait.since >= kPictureGiveUp) {
			pictureWait.gaveUp = true;
			logger::warn("portal: no doorway picture for door {:08X} from the capture camera; merged without", a_portal.sourceDoor);
			return false;
		}
		if (now - *pictureWait.since >= kPictureLightsSettle) {
			pictureWanted = a_portal.sourceDoor;
		}
		return true;
	}

	// ---- the doorway before the space behind it is there ------------------------------------
	// A door used to stay shut until the space behind it had loaded: on a slow computer, walked up
	// to and stood at, shut. A door that is to open (Wanted) now swings open whatever the load, and
	// its doorway shows a blurred picture of what was last seen through it (DoorwaySnapshot), or a
	// plain colour, until the real space takes over and the picture fades out. The door's collision
	// stays: there is nothing to walk into yet. A city gate shows its picture too (StandInSwinging).
	struct DoorwayStandIn
	{
		RE::FormID               linked = 0;
		RE::FormID               swinging = 0;        // of the pair, the one swung open (Doors::SwingingDoor)
		bool                     outdoors = false;    // the space behind is outdoors
		bool                     gate = false;        // outdoors on both sides
		bool                     picture = true;      // its doorway shows the picture (a gate only if set to)
		bool                     handedOver = false;  // the real space took the door over; the picture fades
		Portal::Doors::Swing     swing;               // of the door swinging
		float                    fade = 1.0f;         // the picture's, once handed over
		// The door the player just came out of: its picture is of the space they were standing in a
		// moment ago, and that space fading in over it read as a delay, not as a door opening.
		// Handed over, the picture goes at once.
		bool                     cameThrough = false;
		// Through the door, away from the player, in the door's own cell's coordinates: that cell's 3D
		// moves when it is merged into another space or handed back. Kept as drawn, the street's doors
		// seen from inside Belethor's swung into their buildings once the player walked out (the
		// Bannered Mare's out of sight until its own portal took it over).
		RE::NiPoint3             inwardInCell;
		Portal::Doors::OpenState open;  // solid: the collision stays
	};
	std::unordered_map<RE::FormID, DoorwayStandIn> standIns;  // by the door in the player's space
	constexpr float                                 kStandInFadeSeconds = 0.4f;

	// The doors that may stand open, found a few times a second: the references around the player
	// within kStandInReach, the doors set to stand open always and those used, wherever they are.
	struct StandInDoor
	{
		RE::FormID door;
		RE::FormID linked;
		bool       outdoors;  // the space behind is outdoors
	};
	std::vector<StandInDoor> standInDoors;
	float                    standInScanTimer = 0.0f;
	constexpr float          kStandInScanSeconds = 0.5f;
	// As far as a city's gate can be seen from inside it (Whiterun's from its market, Solitude's
	// down its main street), or the doors of a street forced open.
	constexpr float          kStandInReach = 6000.0f;
	// The player's cell standInDoors was found from. Walked through a door, the list from the space
	// left still held the door just used, its far side now the player's own room: until the next
	// scan it got a stand-in, and walked through backwards, the doorway showed the space the player
	// stood in (the room's picture looking out of it, the street's looking into it).
	RE::TESObjectCELL*       standInScanCell = nullptr;
	// The space merged into the player's when standInDoors was found (InView). Walked into a shop,
	// the scan at the cell change ran before the street was merged into it: the street's doors
	// that stand open (the Bannered Mare's) were seen shut until the next scan, half a second on.
	RE::TESObjectCELL*       standInScanMerged = nullptr;

	// The side a stand-in's door swings away from (a_viewer's), in the player's space: Swing reads
	// it against the door as drawn, which for a door of the merged space isn't where its own cell
	// puts it.
	RE::NiPoint3 AwayFrom(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer)
	{
		const auto c = Portal::Doors::ShownQuad(a_door, a_viewer);
		auto       normal = (c[1] - c[0]).Cross(c[2] - c[0]);
		normal.z = 0.0f;
		normal.Unitize();
		return normal.Dot(Portal::Doors::ShownCenter(a_door) - a_viewer) >= 0.0f ? normal : normal * -1.0f;
	}

	// How far behind a_gate (along a_inward) its far gate stands. By the references' origins: the
	// far gate's 3D isn't loaded yet. Both gates are placed the same way about their meshes, so
	// their origins stand apart as their doorways do.
	float GateDepth(RE::TESObjectREFR* a_gate, RE::FormID a_farGate, const RE::NiPoint3& a_inward)
	{
		return (Lookup(a_farGate)->GetPosition() - a_gate->GetPosition()).Dot(a_inward);
	}
	// Gates this close are one gateway seen from either side (Whiterun's, 20 units apart, one mesh
	// at two sizes). Solitude's stand 370 apart, the player's worldspace walling the passage up
	// between them.
	constexpr float kGatesApart = 150.0f;

	// Which of the pair a stand-in swings: the one the portal would (Doors::SwingingDoor), its
	// picture showing the far one swung; but a gate without a picture swings its own, or the
	// gateway stood empty.
	// Through a city gate the player's own worldspace shows its low-detail copy of the city: drawn
	// in 3D, it sits right from any distance and angle, under the real sky and light. A picture
	// (one taken from 1000 units out, reprojected with its depth) smeared the gatehouse's walls
	// from close by and never looked like the street behind. Where that worldspace walls the
	// passage up (Solitude's gatehouse), the gate mask takes the wall away (StandInGatePassage). A
	// gate set to (Doors::PictureFromAfar) shows its picture, or the plain colour until it has one.
	RE::FormID StandInSwinging(RE::TESObjectREFR* a_door, DoorwayStandIn& a_standIn)
	{
		const auto id = a_door->GetFormID();
		const auto key = Portal::Doors::CorrectionDoor(Portal::Doors::Side(a_door), { a_standIn.linked, a_standIn.outdoors });
		a_standIn.picture = !a_standIn.gate || Portal::Doors::PictureFromAfar(key);
		return a_standIn.picture ? Portal::Doors::SwingingDoor(Portal::Doors::Side(a_door), { a_standIn.linked, a_standIn.outdoors }) : id;
	}

	// Shut again, the stand-in's door and, when the far door is the one swung, that one too: taken
	// over from a let-go (TakeOver), nothing else puts it back, and its cell's 3D, reused, showed
	// it standing open.
	void UnswingStandIn(RE::FormID a_door, const DoorwayStandIn& a_standIn)
	{
		Portal::Doors::RestoreLeaves(a_door);
		if (a_standIn.swinging && a_standIn.swinging != a_door && a_standIn.swinging != openDoorSwung) {
			Portal::Doors::RestoreLeaves(a_standIn.swinging);
		}
	}

	void DropStandIns()
	{
		for (auto& [door, standIn] : standIns) {
			if (!standIn.handedOver) {
				UnswingStandIn(door, standIn);
			}
			Portal::Doors::Close(standIn.open);
		}
		standIns.clear();
	}

	// In the space the player is in: their cell, or outdoors any cell of the same worldspace (the
	// cells around are walked between without a door, so their doors are as near as the cell's own).
	bool InPlayerSpace(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here)
	{
		return Engine::SameSpace(a_door ? a_door->GetParentCell() : nullptr, a_here);
	}

	// In the player's space, or in the space merged into it (seen through a door): there a door set
	// to stand open always was drawn shut, the street seen from inside a shop showing the inn
	// across it with its door closed.
	bool InView(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here)
	{
		return InPlayerSpace(a_door, a_here) || (remote.Merged() && a_door && std::ranges::find(remote.Cells(), a_door->GetParentCell()) != remote.Cells().end());
	}

	void ScanStandInDoors(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here)
	{
		standInDoors.clear();
		const auto add = [&](RE::TESObjectREFR* a_door) {
			const auto linked = PortalLink(a_door, a_here);
			// A door out of a building shows the street only live (StreetShownLive).
			if (!linked || Portal::Doors::StreetShownLive(a_door, Portal::Doors::Outdoors(linked.get())) || Portal::Doors::PortalOff(a_door->GetFormID()) || Portal::Doors::PortalOff(linked->GetFormID()) ||
				std::ranges::any_of(standInDoors, [&](const StandInDoor& a_known) { return a_known.door == a_door->GetFormID(); })) {
				return;
			}
			standInDoors.push_back({ a_door->GetFormID(), linked->GetFormID(), Portal::Doors::Outdoors(linked.get()) });
		};
		RE::TES::GetSingleton()->ForEachReferenceInRange(a_player, kStandInReach, [&](RE::TESObjectREFR* a_ref) {
			add(a_ref);
			return RE::BSContainer::ForEachResult::kContinue;
		});
		// Stored under one door of the pair: either may be the one in the player's space.
		auto doors = Portal::Doors::AlwaysOpenDoors();
		std::ranges::copy(Portal::DoorUse::UsedDoors(), std::back_inserter(doors));
		for (const auto id : doors) {
			auto* door = Lookup(id);
			for (auto* candidate : { door, Portal::Doors::LinkedOf(door) }) {
				if (InView(candidate, a_here) && candidate->Get3D()) {
					add(candidate);
				}
			}
		}
	}

	void UpdateStandIns(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta)
	{
		auto* merged = remote.Merged() ? remote.Cell() : nullptr;
		if ((standInScanTimer -= a_delta) <= 0.0f || a_here != standInScanCell || merged != standInScanMerged) {
			standInScanTimer = kStandInScanSeconds;
			standInScanCell = a_here;
			standInScanMerged = merged;
			ScanStandInDoors(a_player, a_here);
		}
		const auto real = SpaceShown;
		const auto origin = a_player->GetPosition();
		for (const auto& candidate : standInDoors) {
			auto* door = Lookup(candidate.door);
			// Not a door the portal holds open itself (swinging shut after its space was let go).
			if (standIns.contains(candidate.door) || !door || !door->Get3D() || real(candidate.door) || (openDoor.door == candidate.door && !openSwing.Shut()) ||
				!Portal::Doors::Wanted(door, { candidate.linked, candidate.outdoors })) {
				continue;
			}
			auto& standIn = standIns[candidate.door];
			standIn.linked = candidate.linked;
			standIn.outdoors = candidate.outdoors;
			standIn.gate = candidate.outdoors && Portal::Doors::Outdoors(door);
			standIn.cameThrough = Portal::Travel::ArrivalDoor() == candidate.door;
			standIn.swinging = StandInSwinging(door, standIn);
			standIn.swing.TakeOver(standIn.swinging);
			if (Portal::Doors::StandsOpen(door, { candidate.linked, candidate.outdoors })) {
				standIn.swing.SnapOpen();
			}
			standIn.inwardInCell = Portal::Doors::ToShown(door).rotate.Transpose() * AwayFrom(door, origin);
		}
		for (auto it = standIns.begin(); it != standIns.end();) {
			const auto id = it->first;
			auto&      standIn = it->second;
			auto*      door = Lookup(id);
			if (standIn.handedOver) {
				const bool portalHolds = openDoor.door == id && !openSwing.Shut();
				if (!real(id) && !portalHolds) {
					// The real space went while the picture was fading (the portal let go): the stand-in
					// holds the door again, from where the portal left it. Erased instead, the door had
					// no opener until the next frame made a new one: the box behind its panel and a
					// hidden near door back for a frame.
					standIn.handedOver = false;
					standIn.fade = 1.0f;
					standIn.swing.TakeOver(standIn.swinging);
				} else {
					standIn.fade -= a_delta / kStandInFadeSeconds;
					if (standIn.fade <= 0.0f || !real(id)) {
						Portal::Doors::Close(standIn.open);
						it = standIns.erase(it);
					} else {
						++it;
					}
					continue;
				}
			}
			if (!door || !door->Get3D() || !InView(door, a_here)) {
				UnswingStandIn(id, standIn);
				Portal::Doors::Close(standIn.open);
				it = standIns.erase(it);
				continue;
			}
			if (real(id)) {
				// The real space is there: whoever opens the door for it now takes it over where the
				// stand-in left it (the merged room's own door, standing in the same place, among them).
				standIn.swing.LetGo(standIn.swinging);
				standIn.handedOver = true;
				standIn.fade = standIn.cameThrough ? 0.0f : 1.0f;
				++it;
				continue;
			}
			const Portal::Doors::Side farSide{ standIn.linked, standIn.outdoors };
			standIn.swing.Step(door, Portal::Doors::Wanted(door, farSide), a_delta);
			if (Portal::Doors::StandsOpen(door, farSide)) {
				standIn.swing.SnapOpen();
			}
			if (standIn.swing.Shut()) {
				UnswingStandIn(id, standIn);
				Portal::Doors::Close(standIn.open);
				it = standIns.erase(it);
				continue;
			}
			// The menu may have turned it round meanwhile.
			standIn.swinging = StandInSwinging(door, standIn);
			const bool farSwings = standIn.swinging != id;
			Portal::Doors::Open(door, standIn.open, { .hide = farSwings, .solid = true });
			if (farSwings) {
				// Its door is the one behind, not there yet: this one is hidden, and the picture shows
				// the doorway open (the far door swung in it, if it was taken while that stood open).
				Portal::Doors::RestoreLeaves(id);
			} else {
				auto*      linked = Lookup(standIn.linked);
				const auto settings = linked ? Portal::Doors::CorrectionDoor(door, linked)->GetFormID() : id;
				// A gate without a picture swings its own leaves where the portal swings the far
				// gate's: into the space behind. Swung towards the player instead, the leaves jumped
				// from one side of the gateway to the other when the portal took the gate over. Not
				// where the gates stand apart: there its leaves would swing into the masked passage.
				const bool intoFar = standIn.gate &&
				                     Portal::Doors::SwingingDoor({ id, true }, { standIn.linked, true }) == standIn.linked &&
				                     GateDepth(door, standIn.linked, AwayFrom(door, origin)) <= kGatesApart;
				const auto away = Portal::Doors::ToShown(door).rotate * standIn.inwardInCell;
				standIn.swing.Turn(door, door, intoFar ? away : away * -1.0f, settings);
			}
			++it;
		}
	}

	// The amount to start swinging a door at for the space behind it: where a stand-in holds it, or
	// where it was let go of. The space becomes ready in the middle of a frame (RemoteCell::Update),
	// after the stand-ins were updated: the portal's door handling took the door that frame, before
	// the stand-in had let go of it, and swung it open again from shut.
	void TakeOverDoor(Portal::Doors::Swing& a_swing, RE::FormID a_door)
	{
		for (auto& [id, standIn] : standIns) {
			if (!standIn.handedOver && standIn.swinging == a_door) {
				standIn.handedOver = true;
				standIn.fade = standIn.cameThrough ? 0.0f : 1.0f;
				standIn.swing.LetGo(a_door);
				break;
			}
		}
		a_swing.TakeOver(a_door);
	}

	// The street's distant land, objects and trees (BGSTerrainManager's nodes under "LODRoot"),
	// shown in the interior and moved with the street.
	struct MergedLOD
	{
		RE::NiPointer<RE::NiNode>                                   root;
		RE::NiPointer<RE::NiNode>                                   parent;  // its own (the world scene)
		RE::NiTransform                                             local;
		std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, bool>> culled;
		// The persistent references' nodes (RemoteCell::AdoptPersistent) moved and hung in with the
		// street, and where they were.
		struct Moved
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiPointer<RE::NiNode>     parent;
			RE::NiTransform               local;
		};
		std::vector<Moved>        persistent;
		RE::NiTransform           toSource;
		RE::NiPointer<RE::NiNode> branch;  // the interior's, where the street hangs (null: under the world scene)
		Portal::MovedBounds       bounds;  // of the LOD root (the engine's outdoor cells with it) and the persistent nodes
	};
	std::optional<MergedLOD> mergedLOD;

	// Hangs a_object into the interior's branch where it is drawn now.
	void HangInto(RE::NiNode* a_branch, RE::NiAVObject* a_object)
	{
		RE::NiPointer<RE::NiAVObject> keep(a_object);
		const auto                    world = a_object->world;
		if (a_object->parent) {
			a_object->parent->DetachChild(a_object);
		}
		a_branch->AttachChild(a_object, true);
		a_object->local = a_branch->world.Invert() * world;
		RE::NiUpdateData placed;
		a_object->Update(placed);
	}

	// A town's houses, porches and paving are persistent references: their 3D hangs in a node of
	// the worldspace's persistent cell, not in the cells merged with the street, and moved with
	// none of them. Walked out and back in (the engine's grid had them, and took them off when
	// the player went inside), Belethor's porch and the market's paving were missing from the view
	// out, the bare land showing in their place. Nodes adopted later are taken on here as well.
	void MergePersistent()
	{
		if (!mergedLOD) {
			return;
		}
		int taken = 0;
		for (auto& node : remote.BorrowedPersistent()) {
			const bool known = std::ranges::any_of(mergedLOD->persistent, [&](const MergedLOD::Moved& a_moved) { return a_moved.node == node; });
			// Only while it still hangs where AdoptPersistent put it. 3D we loaded (PersistentIn) can
			// be hung into its cell by the engine afterwards (a mod's persistent references placed in
			// the street's cell): it moves with the cell, and moved here as well it stood the move's
			// distance beyond, out of sight (Markarth's mines: barrels, crates, pipes, scaffolding).
			if (known || node->parent != remote.Scene()) {
				continue;
			}
			mergedLOD->persistent.push_back({ node, RE::NiPointer<RE::NiNode>(node->parent), node->local });
			mergedLOD->bounds.Capture(node.get());
			mergedLOD->bounds.UnfixBeforeMove(node.get());
			node->local = mergedLOD->toSource * node->local;
			// HangInto keeps the node where it is drawn now: not updated, that was still the street's
			// place, and the moved ground and rocks stood there, out of the doorway's view.
			RE::NiUpdateData update;
			node->Update(update);
			mergedLOD->bounds.Move(node.get(), mergedLOD->toSource);
			if (mergedLOD->branch) {
				HangInto(mergedLOD->branch.get(), node.get());
			}
			Portal::SettleMotion(node.get());
			++taken;
		}
		if (taken) {
			logger::info("portal: {} node(s) of persistent references merged with the street", taken);
		}
	}

	// Indoors the engine draws the sky's dome only in an interior that shows the sky (Sky::mode
	// kSkyDomeOnly: the Bannered Mare). Elsewhere (Understone Keep) the sky is in interior mode and
	// culled, and the street merged in had none behind it, only the clear colour. Such an interior
	// is given the dome the way the engine gives it to one that shows the sky (Sky::SetMode), for
	// as long as the street is merged.
	struct SkyModeChange
	{
		RE::TESObjectCELL* interior;
		RE::Sky::Mode      before;
	};
	std::optional<SkyModeChange> skyModeChange;

	void SetSkyMode(RE::Sky::Mode a_mode)
	{
		static REL::Relocation<void (*)(RE::Sky*, std::uint32_t)> setMode{ REL::RelocationID(25679, 26222) };
		setMode(RE::Sky::GetSingleton(), static_cast<std::uint32_t>(a_mode));
	}

	void ShowSkyDome()
	{
		auto* sky = RE::Sky::GetSingleton();
		if (skyModeChange || sky->mode.get() != RE::Sky::Mode::kInterior) {
			return;
		}
		skyModeChange = SkyModeChange{ RE::PlayerCharacter::GetSingleton()->GetParentCell(), sky->mode.get() };
		SetSkyMode(RE::Sky::Mode::kSkyDomeOnly);
	}

	void RestoreSkyMode()
	{
		if (!skyModeChange) {
			return;
		}
		// Only in the interior it was changed for: walked out, the engine has set the outdoors' mode.
		if (RE::PlayerCharacter::GetSingleton()->GetParentCell() == skyModeChange->interior && RE::Sky::GetSingleton()->mode.get() == RE::Sky::Mode::kSkyDomeOnly) {
			SetSkyMode(skyModeChange->before);
		}
		skyModeChange.reset();
	}

	// a_door: the interior's door the street is seen through.
	void MergeLOD(const RE::NiTransform& a_toSource, RE::TESObjectREFR* a_door)
	{
		Portal::StreetLOD::Show(remote.Cell(), a_toSource.Invert(), true);
		ShowSkyDome();
		auto* land = RE::BGSTerrainManager::GetLODRoot().get();
		auto* root = land ? land->parent : nullptr;
		if (!root || !root->parent) {
			logger::warn("portal: no LOD root to show in the interior");
			return;
		}
		MergedLOD state{ RE::NiPointer<RE::NiNode>(root), RE::NiPointer<RE::NiNode>(root->parent), root->local, {} };
		for (auto* node : { RE::BGSTerrainManager::GetLODLandRoot().get(), RE::BGSTerrainManager::GetLODObjectRoot().get(), RE::BGSTerrainManager::GetTreeNode().get() }) {
			if (node) {
				state.culled.emplace_back(RE::NiPointer<RE::NiAVObject>(node), node->GetAppCulled());
				node->SetAppCulled(false);
			}
		}
		// The LOD root's parent is the engine's objects root (TES::objRoot), which holds the outdoor
		// grid's cells as well when the player walked in from the street: everything in it moves.
		const auto moveBy = a_toSource * root->local.Invert();
		state.bounds.Capture(root);
		state.bounds.UnfixBeforeMove(root);
		root->local = a_toSource;
		// Updated before HangInto below, which keeps it where it is drawn now.
		RE::NiUpdateData update;
		root->Update(update);
		state.bounds.Move(root, moveBy);
		// Hung into the interior's statics branch (the one holding the most shapes), the street and
		// the LOD are culled into the main view with the interior's own contents. Left under the
		// world scene, only the sky was drawn through the doorway.
		// An interior built of rooms (Riften's jail) is culled through its portal graph: what lies
		// outside its rooms is tested against the view through the rooms' portals, and the street
		// beyond the door is outside all of them (only the sky, the water and the street's
		// particles were drawn). There the street hangs in the room of the door it is seen through,
		// and is culled with that room's view.
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		auto* loaded = here ? here->GetRuntimeData().loadedData : nullptr;
		auto* interior3D = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
		RE::NiNode* branch = nullptr;
		std::size_t most = 0;
		for (auto* up = a_door && a_door->Get3D() ? a_door->Get3D()->parent : nullptr; up && !branch; up = up->parent) {
			if (auto* room = netimmerse_cast<RE::BSMultiBoundRoom*>(up)) {
				branch = room;
				most = room->GetChildren().size();
			}
		}
		if (interior3D && !branch) {
			for (auto& child : interior3D->GetChildren()) {
				auto* node = child ? child->AsNode() : nullptr;
				if (node && node->GetChildren().size() > most) {
					most = node->GetChildren().size();
					branch = node;
				}
			}
		}
		// In a room, the tree LOD (464 instanced shapes looking out of Mistveil Keep at Riften) came
		// into the sun's shadow map (Interior Sun) twice now and then; that batch's list then looped,
		// its last pass leading back to its first, and the game hung drawing it. Not through the
		// shadow maps' own culls of the LOD (left out, it still came in), nor through the main view's
		// lists (the shapes never pass through them): what adds them twice isn't known. Without the
		// distant trees the view out lacks only those (the street's own trees are real ones).
		if (netimmerse_cast<RE::BSMultiBoundRoom*>(branch)) {
			RE::BGSTerrainManager::GetTreeNode()->SetAppCulled(true);
		}
		if (branch) {
			HangInto(branch, root);
			for (auto* cell : remote.Cells()) {
				auto* cellLoaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = cellLoaded ? cellLoaded->cell3D.get() : nullptr) {
					HangInto(branch, cell3D);
				}
			}
			state.branch.reset(branch);
		}
		state.toSource = a_toSource;
		logger::info("portal: distant LOD shown in the interior{}", branch ? std::format(", it and the street hung into the interior's {} of {} objects", netimmerse_cast<RE::BSMultiBoundRoom*>(branch) ? "room at the door" : "branch", most) : std::string());
		// The upscaler's motion vectors: moved in one step, the distant land and objects reached back to
		// where they were drawn outdoors, and with DLSS the view out was low resolution and shimmered.
		Portal::SettleMotion(root);
		mergedLOD = std::move(state);
		MergePersistent();
	}

	void UnmergeLOD()
	{
		const Diag::Scope scope("unmerge LOD");
		Portal::StreetLOD::Hide();
		RestoreSkyMode();
		if (!mergedLOD) {
			return;
		}
		for (auto& [node, parent, local] : mergedLOD->persistent | std::views::reverse) {
			if (node->parent != parent.get()) {
				if (node->parent) {
					node->parent->DetachChild(node.get());
				}
				parent->AttachChild(node.get(), true);
			}
			node->local = local;
			RE::NiUpdateData update;
			node->Update(update);
			Portal::RecomputeBounds(node.get());
			Portal::SettleMotion(node.get());
		}
		for (auto& [node, culled] : mergedLOD->culled) {
			node->SetAppCulled(culled);
		}
		auto& root = mergedLOD->root;
		if (root->parent != mergedLOD->parent.get()) {
			if (root->parent) {
				root->parent->DetachChild(root.get());
			}
			mergedLOD->parent->AttachChild(root.get(), true);
		}
		root->local = mergedLOD->local;
		RE::NiUpdateData update;
		root->Update(update);
		Portal::RecomputeBounds(root.get());
		Portal::SettleMotion(root.get());
		mergedLOD->bounds.Restore();
		mergedLOD.reset();
	}

	// Merged, the player walks into the doorway until the camera has followed them through it;
	// but behind an exterior door is the building's own collision (its volume is solid). Whatever
	// blocks the way in is made non-collidable while the player is in the doorway.
	std::vector<Portal::Doors::OpenState> passage;
	float                                 passageCheck = 0.0f;

	// With the building's collision off, its floor (the threshold, often the porch too) is gone as
	// well, and the player dropped up to 90 units onto the ground under the building. While the
	// passage is open they are held at the height they had when it opened: no gravity, and no
	// falling as far as the engine's fall time and damage are concerned.
	std::optional<float> passageGravity;  // the controller's own value, to put back
	float                passageFloor = 0.0f;
	bool                 passageHeldThisFrame = false;

	void HoldPassageHeight(RE::PlayerCharacter* a_player, bool a_hold)
	{
		auto* controller = a_player->GetCharController();
		if (!controller) {
			// The saved value is kept for when the controller is back (a crossing's teleport can take
			// it away for a frame). Dropped here, the next hold saved the 0 it had set itself, and the
			// player walked out of the doorway with no gravity for good.
			return;
		}
		if (a_hold) {
			passageHeldThisFrame = true;
			if (!passageGravity) {
				passageGravity = controller->gravity;
				passageFloor = a_player->GetPositionZ();
			}
			controller->gravity = 0.0f;
			controller->fallTime = 0.0f;
			controller->fallStartHeight = passageFloor;
			RE::hkVector4 velocity;
			controller->GetLinearVelocityImpl(velocity);
			if (velocity.quad.m128_f32[2] < 0.0f) {
				velocity.quad.m128_f32[2] = 0.0f;
				controller->SetLinearVelocityImpl(velocity);
			}
		} else if (passageGravity) {
			controller->gravity = *passageGravity;
			passageGravity.reset();
		}
	}

	void ClosePassage()
	{
		HoldPassageHeight(RE::PlayerCharacter::GetSingleton(), false);
		for (auto& open : passage) {
			Portal::Doors::Close(open);
		}
		passage.clear();
	}

	RE::TESObjectREFR* Blocker(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float& a_fraction)
	{
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick;
		pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCharController);
		if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
			return nullptr;
		}
		a_fraction = pick.rayOutput.hitFraction;
		return RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
	}

	void OpenPassage(RE::PlayerCharacter* a_player, const Portal::Doors::Mapping& a_mapping, float a_halfHeight)
	{
		auto* cell = a_player->GetParentCell();
		auto* world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			return;
		}
		// Waist and knee height across the doorway, from just in front of it to a room's depth.
		const float floor = a_mapping.sourceCenter.z - a_halfHeight;
		for (const float height : { 70.0f, 30.0f }) {
			auto from = a_mapping.sourceCenter - a_mapping.inward * 20.0f;
			auto to = a_mapping.sourceCenter + a_mapping.inward * 250.0f;
			from.z = to.z = floor + height;
			for (int attempt = 0; attempt < 8; ++attempt) {
				float fraction = 1.0f;
				auto* blocker = Blocker(world, from, to, fraction);
				if (!blocker || blocker == a_player) {
					break;
				}
				const bool ofRoom = std::ranges::find(remote.Cells(), blocker->GetParentCell()) != remote.Cells().end();
				if (Portal::Doors::IsOpen(blocker->GetFormID()) || ofRoom) {
					// (Also the merged room's own collision copied into the world, MergeCollision: it
					// is meant to be there.)
					// A door held open can still stop the ray (Whiterun's doors have a body the layer
					// change doesn't reach) though not the player: what blocks the way is behind
					// it, and with the ray ending here it was never found (the Bannered Mare and the
					// houses around it couldn't be entered). The search goes on just past the hit: the
					// wall closing Sadri's Used Wares' doorway in Windhelm stands 2 units behind its
					// door's back face, and a step of 2 started the next ray on it, which missed it.
					from = from + (to - from) * fraction + a_mapping.inward * 0.1f;
					if ((to - from).Dot(a_mapping.inward) < 5.0f) {
						break;
					}
					continue;
				}
				if (blocker->As<RE::Actor>()) {
					// Someone standing in the doorway: they block it as anywhere else (made
					// non-collidable they could be walked through). Looked for again shortly.
					break;
				}
				logger::info("portal: doorway passage through {:08X} '{}'", blocker->GetFormID(), blocker->GetBaseObject() ? blocker->GetBaseObject()->GetName() : "");
				Portal::Doors::Open(blocker, passage.emplace_back());
			}
		}
	}

	// ---- image space across the doorway --------------------------------------------------------
	// An interior and the outdoors have different image spaces (tone mapping, brightness, tint);
	// the engine switches when the player changes space. Through an open doorway that is a jump:
	// the room seen from the porch is drawn with the outdoor one, and darkens at once on walking
	// in. Instead the two are blended by where the camera is relative to the doorway, on both
	// sides of the crossing, through the manager's override.
	RE::ImageSpaceBaseData toneBlend;
	// The outdoors' as the engine last had it, with the weather and the hour it was taken in: it
	// holds for those. Kept for good, a save loaded indoors at night after a noon outdoors blended
	// the doorway with noon's, and lit the street seen from the room by noon's sun.
	RE::ImageSpaceBaseData toneOutdoors;
	const RE::TESWeather*  toneOutdoorsWeather = nullptr;
	float                  toneOutdoorsHour = 0.0f;  // in Calendar's hours passed
	bool                   toneOutdoorsLogged = false;
	RE::ImageSpaceBaseData toneIndoors;            // the interior the player is in, as the engine had it
	RE::TESObjectCELL*     toneIndoorsCell = nullptr;

	void ClearTone()
	{
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (manager && manager->GetRuntimeData().overrideBaseData == &toneBlend) {
			manager->GetRuntimeData().overrideBaseData = nullptr;
		}
	}

	// The outdoors' image space before the player has been outdoors in this session (loaded indoors):
	// the current weather's for this part of the day. Without it the first walk outdoors had no blend
	// at all, and the street came in at twice the room's brightness at once.
	const RE::ImageSpaceBaseData* WeatherImageSpace()
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* weather = sky ? (sky->currentWeather ? sky->currentWeather : sky->defaultWeather) : nullptr;
		if (!weather) {
			return nullptr;
		}
		using Time = RE::TESWeather::ColorTime;
		const float hour = sky->currentGameHour;
		const auto  time = hour < 5.0f || hour >= 21.0f ? Time::kNight : hour < 8.0f ? Time::kSunrise : hour < 17.0f ? Time::kDay : Time::kSunset;
		auto*       imageSpace = weather->imageSpaces[time];
		return imageSpace ? &imageSpace->data : nullptr;
	}

	const RE::ImageSpaceBaseData* OutdoorImageSpace()
	{
		// The hour only counts indoors: outdoors it is taken every frame, except while our own blend
		// is on (UpdateTone), and standing at a doorway for half an hour mustn't swap it for the
		// weather's.
		constexpr float kHoldsForHours = 0.5f;
		const bool      indoors = RE::PlayerCharacter::GetSingleton()->GetParentCell()->IsInteriorCell();
		const bool      holds = toneOutdoorsWeather && toneOutdoorsWeather == RE::Sky::GetSingleton()->currentWeather &&
		                        (!indoors || std::abs(RE::Calendar::GetSingleton()->GetHoursPassed() - toneOutdoorsHour) < kHoldsForHours);
		return holds ? &toneOutdoors : WeatherImageSpace();
	}

	const RE::ImageSpaceBaseData* CellImageSpace(RE::TESObjectCELL* a_cell)
	{
		auto* extra = a_cell ? a_cell->extraList.GetByType<RE::ExtraCellImageSpace>() : nullptr;
		return extra && extra->imageSpace ? &extra->imageSpace->data : nullptr;
	}

	// a_interiorDepth: how far the camera is inside the interior behind the doorway (negative: outside).
	// a_inDoorway: 1 with the camera in front of the doorway, falling to 0 beside, above or below it,
	// where the player's own space's image space applies alone.
	void UpdateTone(RE::TESObjectCELL* a_here, RE::TESObjectCELL* a_behindDoor, float a_interiorDepth, float a_inDoorway)
	{
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (!manager || !a_here || !a_behindDoor || a_here->IsInteriorCell() == a_behindDoor->IsInteriorCell() ) {
			ClearTone();
			return;
		}
		auto&      runtime = manager->GetRuntimeData();
		const bool ours = runtime.overrideBaseData == &toneBlend;
		if (runtime.overrideBaseData && !ours) {
			return;  // someone else's override (a script, the console)
		}
		const bool inside = a_here->IsInteriorCell();
		// Each side's own settings, never what the engine shows while our blend is on: that is our
		// blend itself. Taken from it, every frame blended the last frame's blend again, and when
		// the blend switched off the picture jumped back: the whole screen flashed near a doorway
		// (in first person, whose camera stays in the blend's range).
		// The outdoors' own only once the player has been out a while: taken in the first frames of the
		// first walk outdoors in a session, they were the engine's in-between values, and the blend
		// made of them darkened the street to near black for a second after it.
		static int framesOutdoors = 0;
		framesOutdoors = inside ? 0 : framesOutdoors + 1;
		if (!ours && runtime.currentBaseData && (inside || framesOutdoors > 30)) {
			if (inside) {
				toneIndoors = *runtime.currentBaseData;
				toneIndoorsCell = a_here;
			} else {
				toneOutdoors = *runtime.currentBaseData;
				toneOutdoorsWeather = RE::Sky::GetSingleton()->currentWeather;
				toneOutdoorsHour = RE::Calendar::GetSingleton()->GetHoursPassed();
				if (!std::exchange(toneOutdoorsLogged, true)) {
					const auto* weather = WeatherImageSpace();
					const auto& e = toneOutdoors.hdr;
					logger::info("portal: outdoor image space eye adapt {:.2f}/{:.2f} white {:.2f} sun {:.2f} sky {:.2f} brightness {:.2f}; the weather's {:.2f}/{:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
						e.eyeAdaptSpeed, e.eyeAdaptStrength, e.white, e.sunlightScale, e.skyScale, toneOutdoors.cinematic.brightness, weather ? weather->hdr.eyeAdaptSpeed : -1.0f,
						weather ? weather->hdr.eyeAdaptStrength : -1.0f, weather ? weather->hdr.white : -1.0f, weather ? weather->hdr.sunlightScale : -1.0f,
						weather ? weather->hdr.skyScale : -1.0f, weather ? weather->cinematic.brightness : -1.0f);
				}
			}
		}
		const auto* indoors = inside ? (toneIndoorsCell == a_here ? &toneIndoors : CellImageSpace(a_here)) : CellImageSpace(a_behindDoor);
		const auto* outdoorsData = OutdoorImageSpace();
		constexpr float kFrom = -150.0f, kTo = 120.0f;
		float           t = std::clamp((a_interiorDepth - kFrom) / (kTo - kFrom), 0.0f, 1.0f);
		t = t * t * (3.0f - 2.0f * t);
		t = inside ? 1.0f - (1.0f - t) * a_inDoorway : t * a_inDoorway;
		// The eye adaptation walked out with was the room's: the street came in far too bright and
		// took seconds to settle. A load door starts it afresh behind its loading screen; here it is
		// let catch up with the street at once in the first frames outdoors.
		constexpr int kAdaptSnapFrames = 15;
		const bool    snapAdapt = !inside && framesOutdoors <= kAdaptSnapFrames;
		constexpr float kSnapSpeed = 1000.0f;  // times the frame time: past 1, the shader's step is the whole way
		if (!indoors || !outdoorsData || (inside ? t >= 1.0f : t <= 0.0f)) {
			if (snapAdapt && (outdoorsData || ours || runtime.currentBaseData)) {
				if (outdoorsData) {
					toneBlend = *outdoorsData;
				} else if (!ours) {
					toneBlend = *runtime.currentBaseData;
				}
				toneBlend.hdr.eyeAdaptSpeed = kSnapSpeed;
				runtime.overrideBaseData = &toneBlend;
				return;
			}
			ClearTone();
			return;
		}
		const auto mix = [t](float a_out, float a_in) { return a_out + (a_in - a_out) * t; };
		toneBlend = inside ? *indoors : *outdoorsData;  // depth of field and the like: the current space's
		const float* outdoors = reinterpret_cast<const float*>(outdoorsData);
		const float* in = reinterpret_cast<const float*>(indoors);
		float*       blend = reinterpret_cast<float*>(&toneBlend);
		// HDR, cinematic and tint are plain floats (0x40 bytes).
		for (std::size_t i = 0; i < offsetof(RE::ImageSpaceBaseData, depthOfField) / sizeof(float); ++i) {
			// Eye adaptation's speed and strength stay the current space's own. On the first walk outdoors
			// of a session the engine starts its adaptation afresh (the white frame FrameHooks' flash
			// guard replaces), and with these blended it then settled near black for a second.
			const bool adapt = i == offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptSpeed) / sizeof(float) || i == offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptStrength) / sizeof(float);
			if (adapt) {
				continue;
			}
			blend[i] = mix(outdoors[i], in[i]);
		}
		if (snapAdapt) {
			toneBlend.hdr.eyeAdaptSpeed = kSnapSpeed;
		}
		runtime.overrideBaseData = &toneBlend;
	}

	RE::NiPoint3 CameraPosition()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return RE::PlayerCharacter::GetSingleton()->GetPosition();
		}
		// From the local transform the camera's Update just set: the world one follows only in the
		// frame's scene update, and right after a crossing it was still at the door left behind (the
		// image space chosen for there: a room walked out into flashed white for one frame).
		const auto* root = camera->cameraRoot.get();
		return root->parent ? root->parent->world * root->local.translate : root->local.translate;
	}

	// When to carry the player through, and what the camera may do meanwhile:
	// - merged (outside, the room drawn in the world): the player walks into the doorway and the
	//   crossing waits until the camera has followed them through it, so both are inside when the
	//   space changes;
	// - portal pass with a mergeable space on the other side (inside, going out): crossing as the
	//   player reaches the plane; the camera, still inside, is then in the merged room;
	// - portal pass otherwise (interior to interior): a camera left on the far side of a portal
	//   can't be drawn well (the doorway quad's "camera behind" composite, Pictures/Composite.cpp), so it is pulled in to reach
	//   the doorway together with the player (Portal::CameraLimit).
	// Returns the camera's distance limit for this frame.
	// Feet at the doorway's own height: no lower than a step below the panel's bottom edge, no
	// higher than its middle. Everything that decides by where the player stands against a doorway
	// also checks this; horizontally, a flat roof over the door is inside its width and depth.
	bool FeetAtDoorway(float a_feetZ, const Portal::Doors::Mapping& a_mapping)
	{
		const float bottom = a_mapping.sourceCenter.z - a_mapping.halfHeight;
		return a_feetZ >= bottom - 64.0f && a_feetZ <= bottom + a_mapping.halfHeight;
	}

	std::optional<float> Approach(RE::PlayerCharacter* a_player, const Portal::Manager::Active& a_portal, float a_delta)
	{
		playerInRoom = false;  // set again below while it holds
		// In portal mode the camera must still be in front of the portal quad (2 units in front of
		// the panel) when the crossing is carried out.
		constexpr float kCrossLead = 4.0f;
		constexpr float kPullInDistance = 400.0f;
		// Merged, the player behind the doorway is on the exterior's physics (no interior floor or
		// furniture there yet), so not for long, even with the camera turned to look back out.
		constexpr float kMergedMaxDepth = 250.0f;
		// The street merged into the interior: crossing out is as without merging; the doors are the merge's.
		const bool      merged = remote.Merged() && !MergedOutdoors();
		if (!merged && !MergedOutdoors() && openDoor.door != a_portal.sourceDoor) {
			return std::nullopt;
		}
		// A shut door is walked into, not through: the building behind it is left solid, and the
		// player isn't held at the doorway's height against it.
		if (remote.Merged() ? primaryRoom->swing.Shut() : openSwing.Shut()) {
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		const auto found = Portal::Doors::TryCompute(a_portal);
		if (!found) {
			return std::nullopt;
		}
		const auto& mapping = *found;
		const auto  DepthOf = [&](const RE::NiPoint3& a_point) {
			const auto offset = a_point - mapping.sourceCenter;
			return offset.x * mapping.inward.x + offset.y * mapping.inward.y;  // < 0: in front
		};
		const auto  offset = a_player->GetPosition() - mapping.sourceCenter;
		const float depth = DepthOf(a_player->GetPosition());
		const float side = std::abs(offset.x * mapping.inward.y - offset.y * mapping.inward.x);
		// Rooms walked into from outdoors (walkIntoRooms): the player is outdoors, on the room's
		// copied collision, while the doorway can be seen; the game switches into the room only
		// where it can't (nothing on screen changes then), and back outdoors as soon as it can
		// again, or the player comes near the door. Seen from inside, the doorway then always shows
		// the game's own outdoors.
		auto*      here = a_player->GetParentCell();
		const bool switching = walkIntoRooms && crossCooldown <= 0.0f;
		if (merged && remote.CollisionMerged()) {
			const bool passing = depth > -90.0f && depth < 40.0f && side < mapping.halfWidth + 30.0f;
			remote.SetDoorwayPiecesPresent(!passing);
		}
		// Up the room's stairs the house around it is in the way: the parts of its collision above
		// the ground floor (its upper floors, the roof's beams) are bodies the passage through it
		// (OpenPassage) doesn't reach. Upstairs the player is in the room itself.
		const bool upstairs = a_player->GetPositionZ() > mapping.sourceCenter.z - mapping.halfHeight + kUpstairsHeight;
		{
			static RE::FormID lastDoor = 0;
			static float      lastDepth = 0.0f;
			const bool        nearer = lastDoor == a_portal.sourceDoor && depth > lastDepth + 0.05f;
			lastDoor = a_portal.sourceDoor;
			lastDepth = depth;
			approachingFor = nearer ? approachingFor + a_delta : 0.0f;
			const bool heading = approachingFor >= kApproachSeconds;
			// Not while sitting, lying or in a conversation: the switch would carry the player out of it.
			const bool busy = a_player->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal || RE::MenuTopicManager::GetSingleton()->speaker;
			if (switching && !merged && !busy && here && here->IsInteriorCell() && !a_portal.cell->IsInteriorCell() && Mergeable(a_portal.cell, here) && depth < 0.0f &&
				!openSwing.Shut() && !upstairs) {
				const bool inView = DoorwayInView(mapping, kSeenMargin);
				if ((inView && heading && -depth <= kRoomSwitchOutViewDepth) || (-depth <= kRoomSwitchOutDepth && heading)) {
					crossCooldown = 1.0f;
					logger::info("portal: switching outdoors {:.1f} short of the door, side {:.1f}{}", -depth, side, inView ? " (doorway in view)" : "");
					Portal::Travel::Request({ .portal = a_portal, .exact = true });
					return std::nullopt;
				}
			}
		}
		playerInRoom = merged && remote.CollisionMerged() && depth > 0.0f;
		if (playerInRoom) {
			HoldPassageHeight(a_player, false);
			// The house around the room is solid (its collision fills its shell): off while the
			// player is in the room, also when they arrived in it from inside (above).
			if (passage.empty() || (passageCheck -= a_delta) <= 0.0f) {
				passageCheck = 1.0f / 6.0f;
				OpenPassage(a_player, mapping, mapping.halfHeight);
			}
			// Into the room where the doorway can't be seen, a little way in; and always before the
			// portal would be let go (far from its door), which takes the copied collision with it.
			const float doorDistance = a_player->GetPosition().GetDistance(mapping.sourceCenter);
			const bool  tooFar = doorDistance > ReleaseRadius() - 150.0f;
			// Unseen for a while, not just while turning: looking round a room with two doors (or
			// glancing away from one) switched in and out every second or two, each time a stutter.
			doorwayUnseenFor = DoorwayInView(mapping, kUnseenMargin) ? 0.0f : doorwayUnseenFor + a_delta;
			if (switching && ((depth >= kMinRoomDepth && (tooFar || doorwayUnseenFor >= kUnseenSeconds)) || upstairs)) {
				crossCooldown = 1.0f;
				attachHoldoff = 3.0f;
				logger::info("portal: switching into the room at depth {:.1f}, side {:.1f}{}", depth, side, tooFar ? " (far from the door)" : upstairs ? " (upstairs)" : "");
				Portal::Travel::Request({ .portal = a_portal, .exact = true });
			}
			return std::nullopt;
		}
		// Behind the door plane counts too: someone who got past it while a crossing wasn't
		// possible yet (the cooldown) is standing in the void behind the doorway and must still be
		// carried through.
		if (depth < -kPullInDistance || depth > kMergedMaxDepth + 50.0f || side > mapping.halfWidth + 150.0f) {
			// Walked off the passage sideways: its building's collision back on under them.
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		// Only at the doorway's own height. Walking over the flat roof of Whiterun's guard house by
		// the gate was within its door's width and depth, and carried the player inside. (In the
		// passage the player is held at the height they entered it with, HoldPassageHeight.)
		if (!FeetAtDoorway(a_player->GetPositionZ(), mapping)) {
			// A passage through the building (OpenPassage) must not stay open, its collision off,
			// under someone who isn't at the doorway.
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		const float cameraDepth = DepthOf(CameraPosition());
		const bool  pullIn = !merged && !Mergeable(a_portal.cell, a_player->GetParentCell());
		// Only right at the threshold: the building's collision is also the porch and the steps in
		// front of the door, and with it gone earlier the player sank into the ground on the way up.
		// Whatever the mode: a door can stand against a wall that closes the void behind it
		// (Riften's jail; the Hall of the Elements' door up to the Arcanaeum, 4 units behind the
		// door's face, with nothing merged), and the player stopped short of the crossing.
		// Only on the way in: arrived through the door at its marker 9 units in front of it, the
		// player was held 10 units above the floor they were set down over (HoldPassageHeight).
		// Facing in and moving, not getting nearer: someone already stopped by the wall gets
		// no nearer.
		const float yaw = a_player->GetAngleZ();
		const bool  walkingIn = a_player->IsMoving() && std::sin(yaw) * mapping.inward.x + std::cos(yaw) * mapping.inward.y > 0.5f;
		if (depth > -25.0f && side <= mapping.halfWidth + 20.0f && (walkingIn || !passage.empty())) {
			// Seconds, not frames: counted per frame it looked again twice as often at 120 fps.
			if (passage.empty() || (passageCheck -= a_delta) <= 0.0f) {
				passageCheck = 1.0f / 6.0f;  // what the per-frame count came to at 60 fps
				OpenPassage(a_player, mapping, mapping.halfHeight);
			}
		} else if (!passage.empty() && depth < -45.0f) {
			ClosePassage();
		}
		// With the room's collision copied in, its floor carries the player through the doorway.
		if (!passage.empty() && !remote.CollisionMerged()) {
			HoldPassageHeight(a_player, true);
		}
		if (merged && remote.CollisionMerged()) {
			return std::nullopt;  // not carried through at the door (see above)
		}
		const bool  inDoorway = side <= mapping.halfWidth + 20.0f;
		// Merged: behind the door plane the player walks on the exterior's ground inside the
		// building's shell (it sinks or rises there), so only a short way; the camera is eased in
		// on the last steps so that it is through the doorway by then, never closer than
		// kMergedCameraMin (the player stays in the picture).
		constexpr float kMergedCrossDepth = 60.0f;
		constexpr float kMergedCameraMin = 55.0f;
		// The portal quad lies on the door's face, half its thickness in front of the centre the
		// depth is measured from: a thick door (a city gate) is crossed that much earlier, or the
		// camera would be through the quad, looking at the gate's planks, before the player is.
		const float     lead = kCrossLead + mapping.halfDepth;
		// A gate's arrival marker can be within that distance of it: the player, just arrived and
		// standing there, was carried straight back, every two seconds. Only someone walking
		// towards the doorway is carried through.
		static RE::FormID lastDoor = 0;
		static float      lastDepth = 0.0f;
		const bool        approaching = lastDoor == a_portal.sourceDoor && depth > lastDepth + 0.05f;
		lastDoor = a_portal.sourceDoor;
		lastDepth = depth;
		// In first person nothing holds the camera back from the doorway (the stop at its plane,
		// CameraLimit, is the third-person camera's), and it is at the head, a little ahead of the
		// body and bobbing with each step: it got to the doorway's picture before the player did,
		// and from beyond it the room's own view showed the door from its far side, flashing with
		// every step close to the door. The player is carried through as soon as the camera's near
		// plane would reach the picture (behind the panel for a swinging door, in front for a gate).
		bool cameraAtPicture = false;
		if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); !merged && playerCamera && playerCamera->IsInFirstPerson()) {
			const auto* worldCamera = RE::Main::WorldRootCamera();
			const float nearPlane = worldCamera ? worldCamera->GetRuntimeData2().viewFrustum.fNear : 15.0f;
			// A city gate (outdoors to outdoors) keeps its picture in front of its panel (Render::PortalPass::IsGate).
			auto*       here = a_player->GetParentCell();
			const bool  gate = here && !here->IsInteriorCell() && !a_portal.cell->IsInteriorCell();
			const float pictureDepth = gate ? -(mapping.halfDepth + 2.0f) : mapping.halfDepth + 2.0f;
			const auto  cameraOffset = CameraPosition() - mapping.sourceCenter;
			const float cameraSide = std::abs(cameraOffset.x * mapping.inward.y - cameraOffset.y * mapping.inward.x);
			cameraAtPicture = cameraDepth + nearPlane + 2.0f >= pictureDepth && cameraSide <= mapping.halfWidth + 20.0f && inDoorway && approaching;
		}
		const bool      cross = merged ? (depth >= 15.0f && cameraDepth >= 2.0f) || depth >= kMergedCrossDepth :
		                                 (depth >= -lead && inDoorway && (approaching || depth >= 0.0f)) || cameraAtPicture;
		if (crossCooldown <= 0.0f && cross) {
			crossCooldown = 1.0f;
			attachHoldoff = 3.0f;
			logger::info("portal: crossing at depth {:.1f}, side {:.1f}, camera depth {:.1f}{}", depth, side, cameraDepth, merged ? " (merged)" : "");
			Portal::Travel::Request({ .portal = a_portal, .limitCamera = pullIn || merged, .cameraFloor = merged ? kMergedCameraMin : 0.0f });
		}
		if (merged) {
			return inDoorway || depth > 0.0f ? std::optional(kMergedCameraMin + std::max(0.0f, kMergedCrossDepth - depth)) : std::nullopt;
		}
		return pullIn ? std::optional(std::max(0.0f, -depth - lead)) : std::nullopt;
	}

	// The merged room's collision copied into a_world, placed like the room (RemoteCell::MergeCollision).
	void MergeRoomCollision(const Portal::Manager::Active& a_portal, RE::bhkWorld* a_world)
	{
		const auto   mapping = Portal::Doors::Compute(a_portal);
		RE::NiPoint3 inward = mapping.toLinked.rotate * RE::NiPoint3{ mapping.inward.x, mapping.inward.y, 0.0f };
		inward.z = 0.0f;
		inward.Unitize();
		remote.MergeCollision(mapping.toLinked.Invert(), a_world, a_portal.linkedDoor, mapping.toLinked * mapping.sourceCenter, inward);
	}

	// ---- using things in a room walked into ----------------------------------------------------
	// In a room walked into from outdoors the player is still outdoors (RemoteCell::MergeCollision);
	// its people and things are in the room's own Havok world, which the crosshair doesn't look
	// into: nothing could be talked to, opened or taken. The pick is repeated in the room's world
	// along the same line, mapped into the room. Using what it finds switches into the room first
	// (Travel, exact: nothing on screen changes), and the use follows on arrival.
	RE::ObjectRefHandle                   pendingUse;
	std::chrono::steady_clock::time_point pendingUseAt;

	// How far beyond the camera's distance from the player the room's things can be reached
	// (the engine's activation reach is about this).
	constexpr float kUseReach = 180.0f;

	void PickInRoom(RE::CrosshairPickData* a_pick, const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction)
	{
		const auto portal = Portal::Manager::ReadyPortal();
		auto*      roomWorld = remote.Cell() ? remote.Cell()->GetbhkWorld() : nullptr;
		if (!portal || !remote.Merged() || !remote.CollisionMerged() || !roomWorld) {
			return;
		}
		const auto found = Portal::Doors::TryCompute(*portal);
		if (!found) {
			return;
		}
		const auto&        mapping = *found;
		auto*              player = RE::PlayerCharacter::GetSingleton();
		const float        reach = kUseReach + a_origin.GetDistance(player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 120.0f });
		RE::NiPoint3       direction = mapping.toLinked.rotate * a_direction;
		direction.Unitize();
		const RE::NiPoint3 from = mapping.toLinked * a_origin;
		const RE::NiPoint3 to = from + direction * reach;
		const float        scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData    pick;
		pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kItemPick);
		if (!roomWorld->PickObject(pick) || !pick.rayOutput.HasHit()) {
			return;
		}
		auto* ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
		if (!ref || ref == player || ref->GetFormID() == portal->linkedDoor) {
			return;
		}
		const float distance = reach * pick.rayOutput.hitFraction;
		// What the world's own pick found, unless it is further away.
		auto* const raw = reinterpret_cast<std::uint8_t*>(a_pick);
		auto&       target = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x04);
		auto&       targetActor = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x08);
		auto&       point = *reinterpret_cast<RE::NiPoint3*>(raw + 0x10);
		if (target.get() && point.GetDistance(a_origin) < distance) {
			return;
		}
		target = ref->GetHandle();
		targetActor = ref->As<RE::Actor>() ? ref->GetHandle() : RE::ObjectRefHandle();
		point = mapping.toLinked.Invert() * (from + direction * distance);
	}

	// An open doorway has no collision for the crosshair to find its door by (Doors::Open): a door
	// opened by a use is found by its doorway, which the crosshair passes through, to be shut.
	void PickOpenDoorway(RE::CrosshairPickData* a_pick, const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction)
	{
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      here = player->GetParentCell();
		const auto used = Portal::DoorUse::UsedDoors();
		if (used.empty() || !here) {
			return;
		}
		auto direction = a_direction;
		direction.Unitize();
		auto* const raw = reinterpret_cast<std::uint8_t*>(a_pick);
		auto&       target = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x04);
		auto&       targetActor = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x08);
		auto&       point = *reinterpret_cast<RE::NiPoint3*>(raw + 0x10);
		float       nearest = target.get() ? point.GetDistance(a_origin) : kUseReach + a_origin.GetDistance(player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 120.0f });
		RE::TESObjectREFR* found = nullptr;
		RE::NiPoint3       at;
		for (const auto id : used) {
			auto* door = Lookup(id);
			if (!door || !door->Get3D() || !InPlayerSpace(door, here) || !Portal::Doors::IsOpen(id) || !Portal::Doors::ClosableDoor(door)) {
				continue;
			}
			const auto  c = Portal::Doors::ShownQuad(door, a_origin);
			const auto  across = c[1] - c[0];
			const auto  up = c[2] - c[0];
			const auto  normal = across.Cross(up);
			const float facing = normal.Dot(direction);
			if (std::abs(facing) < 1e-4f) {
				continue;
			}
			const float distance = normal.Dot(c[0] - a_origin) / facing;
			if (distance <= 0.0f || distance >= nearest) {
				continue;
			}
			const auto  hit = a_origin + direction * distance;
			const float u = (hit - c[0]).Dot(across) / across.SqrLength();
			const float v = (hit - c[0]).Dot(up) / up.SqrLength();
			if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
				continue;
			}
			nearest = distance;
			found = door;
			at = hit;
		}
		if (found) {
			target = found->GetHandle();
			targetActor = {};
			point = at;
		}
	}

	// PlayerCharacter's crosshair update (ID 40620) calling its pick (ID 26127) at +0x1E7.
	struct CrosshairPickHook
	{
		static void Thunk(RE::CrosshairPickData* a_pick, RE::bhkWorld* a_world, RE::NiPoint3* a_origin, RE::NiPoint3* a_direction)
		{
			func(a_pick, a_world, a_origin, a_direction);
			if (a_pick && a_origin && a_direction) {
				PickInRoom(a_pick, *a_origin, *a_direction);
				PickOpenDoorway(a_pick, *a_origin, *a_direction);
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// PlayerCharacter::ActivatePickRef (ID 40548) using the picked reference (ID 19796 at +0x10D).
	struct UseHook
	{
		static bool Thunk(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_activator, std::uint8_t a_arg2, RE::TESBoundObject* a_object, std::int32_t a_count, bool a_defaultOnly)
		{
			const auto portal = Portal::Manager::ReadyPortal();
			const bool inRoom = a_ref && portal && remote.Merged() && remote.CollisionMerged() &&
			                    std::ranges::find(remote.Cells(), a_ref->GetParentCell()) != remote.Cells().end();
			if (inRoom && a_activator == RE::PlayerCharacter::GetSingleton()) {
				if (crossCooldown <= 0.0f) {
					crossCooldown = 1.0f;
					attachHoldoff = 3.0f;
					pendingUse = a_ref->GetHandle();
					pendingUseAt = std::chrono::steady_clock::now();
					logger::info("portal: {:08X} used in the room walked into: switching into the room first", a_ref->GetFormID());
					Portal::Travel::Request({ .portal = *portal, .exact = true });
				}
				return true;
			}
			return func(a_ref, a_activator, a_arg2, a_object, a_count, a_defaultOnly);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The actor's 3D position update (ID 37995, every frame for each actor in high process, on the
	// job threads) hangs its 3D into its cell's node for actors whenever it hangs elsewhere (ID 19165,
	// which queues the attach). A merged room's people hang directly under their cell's 3D, where the
	// scene's cull reaches them (UpdateMerged): the engine moved them back every frame, and they were
	// hung again every frame. Only UpdateMerged hangs an actor there, so that alone tells them apart,
	// without asking the manager from a job thread. One walked out into the street has a new cell
	// and is hung into it as usual.
	struct ActorHangHook
	{
		static void Thunk(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref, bool a_arg3, bool a_arg4)
		{
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			auto* object = a_ref->Get3D();
			if (loaded && loaded->cell3D && object && object->parent == loaded->cell3D.get()) {
				return;
			}
			func(a_cell, a_ref, a_arg3, a_arg4);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Per frame: the use waiting for the switch into its room.
	void FinishPendingUse()
	{
		auto ref = pendingUse.get();
		if (!ref) {
			pendingUse.reset();
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player->GetParentCell() == ref->GetParentCell()) {
			pendingUse.reset();
			ref->ActivateRef(player, 0, nullptr, 1, false);
		} else if (std::chrono::steady_clock::now() - pendingUseAt > std::chrono::seconds(3)) {
			pendingUse.reset();
		}
	}

	// Using a seamless door opens it, or shuts it again if it was opened by a use; never the
	// engine's trip through it with a loading screen (walking through the open doorway is the
	// trip). One that stands open (a city gate, set to stand open always, all doors forced open)
	// can't be shut. Also right after its lock was picked, when the engine uses the door itself.
	struct DoorActivateHook
	{
		static bool Thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_count)
		{
			if (auto* linked = enabled && a_target && a_activator == RE::PlayerCharacter::GetSingleton() ? Portal::Doors::UsedPairOf(a_target) : nullptr) {
				const auto id = a_target->GetFormID();
				if (Portal::Doors::StandsOpen(a_target, linked)) {
					logger::info("portal: door {:08X} used: stands open", id);
				} else if (Portal::DoorUse::Used(id)) {
					if (Portal::DoorUse::Shut(id)) {
						Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), false);
					}
				} else if (Portal::DoorUse::Use(id, linked->GetFormID())) {
					Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), true);
				}
				return true;
			}
			return func(a_door, a_target, a_activator, a_arg3, a_object, a_count);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The player is about to take a_door the engine's way (its loading screen): the engine gets the
	// cells it leads into, not ones attached under our scene.
	void BeforeEngineTrip(RE::TESObjectREFR* a_door)
	{
		const auto portal = GetActive();
		if (portal && a_door->GetFormID() == portal->sourceDoor) {
			attachHoldoff = 3.0f;
			Portal::Manager::ReleaseAll("player took the portal door the engine's way");
		}
		// A door into a space kept out of sight leads the engine into a cell attached under our
		// scene: it is let go first.
		auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		auto  linked = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		if (auto* cell = linked ? linked->GetParentCell() : nullptr) {
			if (const auto kept = std::ranges::find(warm, cell, &Portal::RemoteCell::Cell); kept != warm.end()) {
				LetGoKept(kept, "player took a door into it");
				attachHoldoff = std::max(attachHoldoff, 3.0f);
			}
		}
	}

	class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
		{
			// Opened or shut (DoorActivateHook), not travelled through.
			if (a_event && a_event->objectActivated && a_event->actionRef.get() == RE::PlayerCharacter::GetSingleton() && Portal::Doors::UsedPairOf(a_event->objectActivated.get())) {
				return RE::BSEventNotifyControl::kContinue;
			}
			// The player used a door the normal way.
			if (a_event && a_event->objectActivated && a_event->actionRef.get() == RE::PlayerCharacter::GetSingleton()) {
				BeforeEngineTrip(a_event->objectActivated.get());
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	} activateSink;

	// A doorway standing open while the space behind it isn't there to walk into (a stand-in: its
	// picture, or a gate onto the player's own worldspace; the space still loading, or not loaded
	// at all after a trip through a city gate, where the view back waits for the purge) is walked
	// through all the same. Its door stays solid, or the player walked on into whatever stands
	// behind it in their own space (past a city gate, the street worldspace's low-detail copy of
	// the city); walking into it, they are taken through the engine's way, with its loading
	// screen. Without this they could only stand at the open doorway and wait.
	struct Pressing
	{
		RE::FormID door = 0;
		float      depth = 0.0f;
		float      held = 0.0f;  // seconds walked into it without getting nearer
	} pressing;

	void WalkIntoStandIns(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta)
	{
		// From the doorway's centre out: the door's panel plus what may stop the player short of it
		// (a city gate's arch reaches well past its panel).
		constexpr float kReach = 120.0f;
		// Held back (by the door, or anything in front of it) rather than still walking up to it.
		constexpr float kHeldSpeed = 30.0f;
		constexpr float kHeldSeconds = 0.15f;
		RE::FormID  door = 0;
		float       depth = 0.0f;
		const float yaw = a_player->GetAngleZ();
		const auto  facing = RE::NiPoint3{ std::sin(yaw), std::cos(yaw), 0.0f };
		const auto  position = a_player->GetPosition();
		if (a_player->IsMoving() && crossCooldown <= 0.0f) {
			for (const auto& [id, standIn] : standIns) {
				auto* ref = Lookup(id);
				if (standIn.handedOver || standIn.swing.Amount() < 0.5f || !ref || !ref->Get3D() || !InPlayerSpace(ref, a_here) || ref->IsLocked()) {
					continue;
				}
				const auto  center = Portal::Doors::Center(ref);
				const auto  size = Portal::Doors::Size(ref);
				const auto  inward = AwayFrom(ref, position);
				const auto  offset = position - center;
				const float d = offset.x * inward.x + offset.y * inward.y;  // < 0: in front
				const float side = std::abs(offset.x * inward.y - offset.y * inward.x);
				const float bottom = center.z - size.halfHeight;
				if (d >= -(size.halfDepth + kReach) && side <= size.halfWidth && position.z >= bottom - 64.0f && position.z <= bottom + size.halfHeight &&
					facing.Dot(inward) > 0.5f) {
					door = id;
					depth = d;
					break;
				}
			}
		}
		if (!door || door != pressing.door) {
			pressing = { door, depth, 0.0f };
			return;
		}
		pressing.held = depth - pressing.depth < kHeldSpeed * a_delta ? pressing.held + a_delta : 0.0f;
		pressing.depth = depth;
		if (pressing.held < kHeldSeconds) {
			return;
		}
		pressing = {};
		crossCooldown = 1.0f;
		auto* ref = Lookup(door);
		logger::info("portal: door {:08X} walked into before the space behind it was there: taken the engine's way", door);
		BeforeEngineTrip(ref);
		Portal::Travel::ThroughDoor(ref);
	}

	class CellSink final : public RE::BSTEventSink<RE::BGSActorCellEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
		{
			// The player arrived in another space (a door transition, a teleport): the engine owns the
			// cells there now. Walking from one exterior cell into the next is not that.
			const auto portal = GetActive();
			if (a_event && a_event->flags.all(RE::BGSActorCellEvent::CellFlag::kEnter) && portal) {
				const auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_event->cellID);
				// Through a city gate the player arrives in the portal's worldspace, not necessarily
				// in the very cell its door is in.
				const bool portalWorld = cell && portal->cell && !cell->IsInteriorCell() && !portal->cell->IsInteriorCell() &&
				                         cell->GetRuntimeData().worldSpace == portal->cell->GetRuntimeData().worldSpace;
				if (cell && (cell->IsInteriorCell() || cell == portal->cell || portalWorld)) {
					Portal::Manager::ReleaseAll("player entered another space");
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	} cellSink;
}

namespace Portal::Manager
{
	namespace
	{
		// What the renderers ask about the shapes they draw (Render::SceneQuery).
		class RenderedScene final : public ::Render::SceneQuery
		{
		public:
			bool IsMergedShape(const RE::BSGeometry* a_shape) const override { return Portal::Manager::IsMergedShape(a_shape); }
			bool IsPlayerShape(const RE::BSGeometry* a_shape) const override { return Portal::Manager::IsPlayerShape(a_shape); }
			int  MergedRoomOf(const RE::BSGeometry* a_shape) const override { return Portal::Manager::MergedRoomOf(a_shape); }

			RE::ShadowSceneNode* MergedRoomScene(int a_slot) const override { return Portal::Manager::MergedRoomScene(a_slot); }

			const std::vector<RE::TESObjectCELL*>& RemoteCells() const override { return Portal::Manager::RemoteCells(); }
		} sceneQuery;
	}

	void Install()
	{
		Portal::Doors::SetHost({ .prompt = Prompt, .hasPicture = Portal::Snapshot::HasPicture, .spaceShown = SpaceShown, .portalLink = PortalLink });
		// Before the fork can ask, whether or not portals are on.
		::Render::RegisterScene(sceneQuery);
		using Plugin::Settings::Id;
		using Plugin::Settings::Bool;
		using Plugin::Settings::Int;
		enabled = Bool(Id::kEnabled);
		mergeEnabled = Bool(Id::kMerge);
		SetDoorOpenDistance(static_cast<float>(Int(Id::kDoorOpenDistance)));
		Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(Int(Id::kForceDoors)));
		Portal::Snapshot::SetPictureHeight(Int(Id::kDoorwayPictureHeight));
		walkIntoRooms = Bool(Id::kWalkIntoRooms);
		mergeOutdoors = Bool(Id::kMergeOutdoors);
		MergedCull::SetMode(static_cast<MergedCull::Mode>(Int(Id::kMergedCull)));
		SetShownRooms(Int(Id::kShownRooms));
		pauseUnseen = Bool(Id::kPauseUnseenRooms);
		// Precision patches the engine's world step loop (ID 77851 + 0x407) with a pre-step hook
		// that takes its own locks for whichever world is stepped; stepping a second world in the
		// step job deadlocked the game in that hook (main thread waiting for the job, ID 35409).
		// Without stepping, NPCs behind the door don't move physically until the player is there.
		// See Settings kStepRemoteHavok for the modes; the default is 2 with Precision loaded
		// (see RemoteCell::StepAtEndOfFrame), else 1.
		const bool stepRemote = Int(Id::kStepRemoteHavok) != 0;
		Spaces::LoadTuning();
		// The hooks below patch engine code in the draw, cull, physics and travel paths. With
		// portals off they did nothing yet were all there, one more thing to clash with other
		// mods' hooks (as with Precision's in the physics step). They can't be put in later when
		// portals are switched on: the physics step's call site runs on job threads, and code
		// can't be patched safely under them. Agent sessions get them always (portals_enable).
		hooksInstalled = enabled || Diag::AgentMode();
		if (!hooksInstalled) {
			logger::info("portal: off in {}; no hooks installed (switching on needs a restart)", Paths::PluginDirectory().string());
			return;
		}
		// Before anything asks whether a room can be merged (Mergeable).
		::Render::Backend::Install();
		Portal::RemoteCell::InstallStepHook(stepRemote);
		Portal::RemoteCell::InstallGridHook();
		Render::Frame::Install();
		Render::Frame::Prewarm();
		Portal::Snapshot::Prewarm();
		Portal::Travel::Install();
		Portal::CameraLimit::Install();
		Portal::StreetLOD::Install();
		MergedCull::Install();
		Portal::Doors::InstallPromptHook();
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESActivateEvent>(&activateSink);
		{
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectDOOR[0] };
			DoorActivateHook::func = vtable.write_vfunc(0x37, DoorActivateHook::Thunk);
		}
		{
			REL::Relocation<std::uintptr_t> pickSite{ REL::RelocationID(39534, 40620), Hooks::Offset(0x214, 0x1D5, 0x1E7) };
			if (Hooks::CallsTo(pickSite.address(), REL::RelocationID(25591, 26127), "portal: crosshair pick")) {
				CrosshairPickHook::func = SKSE::GetTrampoline().write_call<5>(pickSite.address(), CrosshairPickHook::Thunk);
			}
			REL::Relocation<std::uintptr_t> useSite{ REL::RelocationID(39471, 40548), REL::Relocate(0x135, 0x10D) };
			if (Hooks::CallsTo(useSite.address(), REL::RelocationID(19369, 19796), "portal: use of the picked reference")) {
				UseHook::func = SKSE::GetTrampoline().write_call<5>(useSite.address(), UseHook::Thunk);
			}
			REL::Relocation<std::uintptr_t> hangSite{ REL::RelocationID(36970, 37995), REL::Relocate(0x298, 0x279) };
			if (Hooks::CallsTo(hangSite.address(), REL::RelocationID(18683, 19165), "portal: actor hung into its cell")) {
				ActorHangHook::func = SKSE::GetTrampoline().write_call<5>(hangSite.address(), ActorHangHook::Thunk);
			}
		}
		RE::PlayerCharacter::GetSingleton()->AsBGSActorCellEventSource()->AddEventSink(&cellSink);
		logger::info("portal: manager installed, {} ({})", enabled ? "enabled" : "disabled", Paths::PluginDirectory().string());
	}

	bool HooksInstalled()
	{
		return hooksInstalled;
	}

	void SetEnabled(bool a_enabled)
	{
		if (a_enabled && !hooksInstalled) {
			logger::warn("portal: can't be switched on in this session (off in the ini at startup)");
			return;
		}
		enabled = a_enabled;
		logger::info("portal: {}", enabled ? "enabled" : "disabled");
	}

	bool Enabled()
	{
		return enabled;
	}

	namespace
	{
		// Someone who walked out of the merged room: their 3D must not stay under its (moved)
		// cell, or it is drawn tens of thousands of units away until the engine happens to
		// rebuild it; out on the street they were invisible for a second. It goes into the
		// actors' node of the cell they are in now. Also while the room is paused: they walk on.
		void ReturnLeavers(MergedRoom& a_room, const Portal::RemoteCell& a_space)
		{
			std::erase_if(a_room.actors, [&](auto& a_entry) {
				auto& [object, holder] = a_entry;
				auto* owner = object->GetUserData();
				auto* cell = owner ? owner->GetParentCell() : nullptr;
				if (!cell || std::ranges::find(a_space.Cells(), cell) != a_space.Cells().end()) {
					return false;
				}
				auto* loaded = cell->GetRuntimeData().loadedData;
				auto* cellNode = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
				auto* mergedNode = holder->parent;
				if (cellNode && mergedNode && object->parent == mergedNode) {
					// The same slot as the holder's in its own cell (the node of the actors).
					std::uint32_t slot = 0;
					for (auto& child : mergedNode->GetChildren()) {
						if (child.get() == holder.get()) {
							break;
						}
						++slot;
					}
					auto* actors = slot < cellNode->GetChildren().size() && cellNode->GetChildren()[slot] ? cellNode->GetChildren()[slot]->AsNode() : nullptr;
					if (actors) {
						mergedNode->DetachChild(object.get());
						actors->AttachChild(object.get(), true);
						RE::NiUpdateData update;
						object->Update(update);
					}
				}
				return true;
			});
		}

		void UpdateMerged(MergedRoom& a_room, Portal::RemoteCell& a_space, const Active& a_portal, int a_slot, bool a_wanted, float a_delta,
			std::vector<MergedCull::Room>& a_culls);
		void EndMerged(MergedRoom& a_room, int a_slot);
		void SetShownCells(bool a_shown);
		bool DoorShut();
		void PublishRooms();
		void TakeBeside(const RE::TESObjectCELL* a_cell);
		bool DemoteShown(RE::TESObjectCELL* a_here, const Active& a_portal);
		void UpdateBeside(RE::TESObjectCELL* a_here, float a_delta, std::vector<MergedCull::Room>& a_culls);
		Portal::RemoteCell* KeptSpace(const RE::TESObjectCELL* a_cell);
		void UpdateGateMask(RE::TESObjectCELL* a_here);
	}

	namespace
	{
		void ReleaseWarm(std::string_view a_reason)
		{
			while (!warm.empty()) {
				LetGoKept(warm.begin(), a_reason);
			}
		}

		// The interiors to keep warm: the nearest of warmTargets within a_reach, as many as there is
		// room for beside the shown space, not that one.
		std::vector<RE::FormID> WarmWanted(float a_reach)
		{
			std::vector<RE::FormID> wanted;
			const auto              shown = GetActive();
			for (const auto& target : warmTargets) {
				if (std::cmp_greater_equal(wanted.size() + 1, shownRoomsSetting)) {
					break;
				}
				if (target.distance <= a_reach && (!shown || shown->cell->GetFormID() != target.cell)) {
					wanted.push_back(target.cell);
				}
			}
			return wanted;
		}

		// How far the nearest door into a_cell was at the last scan (beyond any reach if none was).
		float WarmDistanceOf(RE::FormID a_cell)
		{
			const auto target = std::ranges::find(warmTargets, a_cell, &WarmTarget::cell);
			return target != warmTargets.end() ? target->distance : FLT_MAX;
		}

		// The same by Rank: which of two rooms is kept.
		float WarmRankOf(RE::FormID a_cell)
		{
			const auto target = std::ranges::find(warmTargets, a_cell, &WarmTarget::cell);
			return target != warmTargets.end() ? target->rank : FLT_MAX;
		}

		std::vector<Portal::RemoteCell>::iterator LetGoWarm(std::vector<Portal::RemoteCell>::iterator a_space, std::string_view a_why)
		{
			return LetGoKept(a_space, a_why);
		}

		void UpdateWarm(RE::TESObjectCELL* a_here, float a_delta)
		{
			if (shownRoomsSetting <= 1 || a_here->IsInteriorCell()) {
				if (!warm.empty()) {
					ReleaseWarm(a_here->IsInteriorCell() ? "player is indoors" : "none kept");
				}
				return;
			}
			// Fewer kept than before (changed in the menu): the farthest go first.
			while (std::cmp_greater_equal(warm.size(), shownRoomsSetting)) {
				LetGoWarm(std::ranges::max_element(warm, {}, [](const Portal::RemoteCell& a_space) { return WarmRankOf(a_space.Cell()->GetFormID()); }),
					"fewer kept");
			}
			for (auto it = warm.begin(); it != warm.end();) {
				if (!it->Healthy()) {
					it = LetGoWarm(it, "no longer attached under its scene");
				} else if (WarmDistanceOf(it->Cell()->GetFormID()) > warmReach + kWarmMargin) {
					it = LetGoWarm(it, "out of reach");
				} else {
					// Merged beside the shown one and seen, it is drawn; any other kept room is out of sight.
					const auto beside = std::ranges::find(besideRooms, it->Cell(), &BesideRoom::cell);
					it->SetPaused(pauseUnseen && (beside == besideRooms.end() || beside->paused));
					it->Update(a_delta);
					++it;
				}
			}
			const auto kept = [](RE::FormID a_cell) {
				return std::ranges::any_of(warm, [&](const Portal::RemoteCell& a_space) { return a_space.Cell()->GetFormID() == a_cell; });
			};
			const auto wanted = WarmWanted(warmReach);
			const auto next = std::ranges::find_if(wanted, [&](RE::FormID a_cell) { return !kept(a_cell) && !(a_cell == failedCell && failedCooldown > 0.0f); });
			if (next == wanted.end() || (warmAttachTimer -= a_delta) > 0.0f || attachHoldoff > 0.0f) {
				return;
			}
			// The space shown, or about to be, loads first.
			if (const auto portal = GetActive(); portal && !(remote.Healthy() && remote.FullyLoaded())) {
				return;
			}
			auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(*next);
			if (!cell) {
				return;
			}
			// No room: the farthest kept room makes way, only for a door clearly nearer than its own.
			// Running along a street the order changes all the time; let go for every change, rooms
			// were attached and let go again a fifth of a second later.
			if (std::cmp_greater_equal(warm.size() + 1, shownRoomsSetting)) {
				const auto farthest = std::ranges::max_element(warm, {}, [](const Portal::RemoteCell& a_space) { return WarmRankOf(a_space.Cell()->GetFormID()); });
				if (farthest == warm.end() || WarmRankOf(*next) + kSwitchMargin >= WarmRankOf(farthest->Cell()->GetFormID())) {
					return;
				}
				LetGoWarm(farthest, "a nearer door");
			}
			warmAttachTimer = kWarmAttachSeconds;
			const auto         started = std::chrono::steady_clock::now();
			Portal::RemoteCell space;
			bool attachedKept = false;
			{
				const Diag::Scope scope("attach kept");
				attachedKept = space.Attach(cell, false, false);
			}
			if (attachedKept) {
				logger::info("portal: attached {} out of sight behind its door ({:.1f} ms)", Probe::DescribeCell(cell),
					std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
				warm.push_back(std::move(space));
			} else {
				failedCell = *next;
				failedCooldown = kRetryCooldown;
				logger::info("portal: could not attach {} out of sight", Probe::DescribeCell(cell));
			}
		}
	}

	// The space shown behind the portal's door let go; the ones kept warm stay.
	void ReleaseShown(std::string_view a_reason, float a_holdoff = 0.0f, bool a_handOver = false);

	namespace
	{
		// The doors NPCs reached for (NpcTravel's door idle, on the AI's threads) open: a seamless
		// door, not a marker, its far end known.
		void NoteActorUses()
		{
			for (const auto id : Portal::DoorUse::TakeActorUses()) {
				auto* door = Lookup(id);
				auto* linked = Portal::Doors::LinkedOf(door);
				auto* base = door ? door->GetBaseObject() : nullptr;
				const bool locked = door && door->IsLocked() && Portal::DoorUse::GetForce() != Portal::DoorUse::Force::kAllOpen;
				if (!linked || !base || static_cast<RE::TESObject*>(base)->IsMarker() || locked || Doors::PortalOff(id) || Doors::PortalOff(linked->GetFormID())) {
					continue;
				}
				if (Portal::DoorUse::Use(id, linked->GetFormID())) {
					Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), true);
				}
			}
		}

		// Both doors of every pair standing (nearly) open, for the AI: an NPC walks through those
		// without reaching for the door.
		void PublishOpenDoors()
		{
			constexpr float         kOpen = 0.9f;
			std::vector<RE::FormID> doors;
			const auto              add = [&](RE::FormID a_door, RE::FormID a_linked) {
				doors.push_back(a_door);
				doors.push_back(a_linked);
			};
			if (const auto portal = GetActive()) {
				if (remote.Merged() ? primaryRoom->swing.Amount() >= kOpen : openDoor.door == portal->sourceDoor && openSwing.Amount() >= kOpen) {
					add(portal->sourceDoor, portal->linkedDoor);
				}
			}
			for (const auto& beside : besideRooms) {
				if (beside.state->swing.Amount() >= kOpen) {
					add(beside.sourceDoor, beside.linkedDoor);
				}
			}
			for (const auto& [id, standIn] : standIns) {
				if (!standIn.handedOver && standIn.swing.Amount() >= kOpen) {
					add(id, standIn.linked);
				}
			}
			Portal::DoorUse::PublishOpen(std::move(doors));
		}
	}

	void ReleaseAll(std::string_view a_reason, float a_holdoff, bool a_handOver)
	{
		{
			const Diag::Scope scope("release kept");
			ReleaseWarm(a_reason);
		}
		const Diag::Scope scope("release shown");
		ReleaseShown(a_reason, a_holdoff, a_handOver);
	}

	void ReleaseShown(std::string_view a_reason, float a_holdoff, bool a_handOver)
	{
		attachHoldoff = std::max(attachHoldoff, a_holdoff);
		if (!remote.Attached() && !GetActive()) {
			return;
		}
		lastEvent = std::format("released: {}", a_reason);
		const auto started = std::chrono::steady_clock::now();
		struct Timing
		{
			std::chrono::steady_clock::time_point started;
			~Timing() { logger::info("portal: release took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count()); }
		} timing{ started };
		logger::info("portal: {}", lastEvent);
		CloseDoor();
		ClosePassage();
		if (remote.Merged()) {
			const Diag::Scope scope("release: end merged");
			EndMerged(*primaryRoom, 0);
		}
		primaryRoom = std::make_unique<MergedRoom>();
		Portal::StreetLOD::Hide();
		SetActive(std::nullopt);
		SetShownCells(false);
		remote.Release(a_handOver);
		PublishRooms();
	}

	void ForgetDoors()
	{
		DropStandIns();
		Portal::Doors::Swing::ForgetAll();
		standInDoors.clear();
		standInScanTimer = 0.0f;
		standInScanCell = nullptr;
		standInScanMerged = nullptr;
		Portal::DoorUse::Clear();
	}

	void Update(float a_delta)
	{
		Portal::WaterReflections::Update();
		approachLimit.reset();
		Portal::Doors::HidePrompt();
		Portal::Doors::Swing::Expire(a_delta);
		if (pendingUse) {
			FinishPendingUse();
		}
		static float forgetTimer = 0.0f;
		if ((forgetTimer -= a_delta) <= 0.0f) {
			forgetTimer = 5.0f;
			Doors::ForgetUnloaded();
		}
		failedCooldown -= a_delta;
		crossCooldown -= a_delta;
		attachHoldoff -= a_delta;
		{
			// Community Shaders' skylighting starts afresh around the street (its probes are kept by
			// position, and the player's jumps from the interior's coordinates to the street's) and
			// fades in over seconds: the street under roofs and porches was far too bright, darkening
			// slowly. A load door hides that behind its loading screen; here it is settled at once.
			auto*       cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			static bool wasInside = false;
			const bool  inside = cell && cell->IsInteriorCell();
			if (cell && wasInside && !inside) {
				::Render::Backend::SettleSkylighting();
			}
			if (cell) {
				wasInside = inside;
			}
		}
		if (!enabled) {
			DropStandIns();
			ReleaseAll("disabled");
			Portal::DoorUse::Clear();
			// Otherwise only cleared at the end of an enabled frame: switched off in a doorway, the
			// blended image space and the camera's stop at the door plane stayed.
			ClearTone();
			Portal::CameraLimit::ClearPlane();
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		if (!here) {
			return;
		}
		if (!here->IsInteriorCell() && Portal::RemoteCell::PersistentToHang()) {
			Portal::RemoteCell::HangHandedPersistent();
		}
		if (remote.Attached() && !remote.Healthy()) {
			ReleaseShown("remote cell no longer attached under the portal");
		}

		TrackHeading(player->GetPosition());
		NoteActorUses();
		for (const auto& [door, linked] : Portal::DoorUse::Update(a_delta)) {
			Portal::DoorUse::PlayDoorSound(door, linked, false);
		}
		const auto candidate = CandidateDoor(player, a_delta);
		if (const auto portal = GetActive()) {
			auto*       door = Lookup(portal->sourceDoor);
			auto*       linkedDoor = Lookup(portal->linkedDoor);
			const float reach = door && linkedDoor ? Reach(player->GetPosition(), door, linkedDoor) : FLT_MAX;
			// Another door into the same space counts too (an inn's kitchen door next to its front
			// door): the space is placed by the door pair, so it has to be the nearest door's.
			const bool  better = candidate && (candidate->cell != portal->cell || candidate->door->GetFormID() != portal->sourceDoor) &&
			                     candidate->rank + kSwitchMargin < (door && linkedDoor ? Rank(player->GetPosition(), door, linkedDoor) : FLT_MAX);
			// A shop closing for the night locks its door.
			const bool  off = Portal::Doors::PortalOff(portal->sourceDoor) || Portal::Doors::PortalOff(portal->linkedDoor);
			const bool  locked = door && door->IsLocked() && Portal::DoorUse::GetForce() != Portal::DoorUse::Force::kAllOpen;
			const char* end = reach > ReleaseRadius() ? "player walked away" : better ? "a closer door" : locked ? "door locked" :
			                  off                         ? "portal switched off for this door" :
			                                                nullptr;
			// Walking in a room walked into, the player stands on its copied collision: it isn't let
			// go under them. Approach switches into the room before the portal's reach ends.
			if (playerInRoom && remote.CollisionMerged()) {
				end = nullptr;
			}
			// A door a stand-in will hold open is let go open, to be taken over where it is: swung shut
			// first, it swung open again right after.
			const bool keepOpen = door && linkedDoor && !off && !locked && Portal::Doors::Wanted(door, linkedDoor);
			doorWanted = keepOpen;
			if (end && (DoorShut() || keepOpen || reach > ReleaseRadius() + kForceReleaseBeyond)) {
				if (better && candidate->cell == portal->cell && !remote.Merged()) {
					// The other door leads into the same space, which is attached already: only the
					// door pair changes. Let go and attached again, the doorway was empty for a third
					// of a second, every time the player walked between a shop's two doors.
					CloseDoor();
					SetActive(Active{ candidate->door->GetFormID(), candidate->linkedDoor, portal->cell, portal->generation });
					doorWanted = false;  // the other door's, until the next frame decides for this one
					// The street's distant land is placed by the door pair, as at the attach: kept, it
					// stood where the first door's pair put it, off the street seen through this one.
					if (const auto mapping = Doors::TryCompute(*GetActive()); mapping && here->IsInteriorCell() && !portal->cell->IsInteriorCell()) {
						Portal::StreetLOD::Show(portal->cell, mapping->toLinked, false);
					}
					lastEvent = std::format("switched to door {:08X} into the same space", candidate->door->GetFormID());
					logger::info("portal: {}", lastEvent);
				} else if (!(better && DemoteShown(here, *portal))) {
					ReleaseShown(end);
				}
			}
		}
		// Before the portal's own door handling: a door a stand-in held is handed over this frame.
		UpdateStandIns(player, here, a_delta);
		WalkIntoStandIns(player, here, a_delta);
		if (!GetActive() && candidate && attachHoldoff <= 0.0f && !(candidate->cell->GetFormID() == failedCell && failedCooldown > 0.0f)) {
			const auto attachStarted = std::chrono::steady_clock::now();
			bool       attached = false;
			if (const auto kept = std::ranges::find(warm, candidate->cell, &Portal::RemoteCell::Cell); kept != warm.end()) {
				// Attached out of sight already (UpdateWarm): only its light scene becomes the drawn one.
				// Merged beside the shown one already, it is drawn on as it was.
				TakeBeside(candidate->cell);
				std::swap(remote, *kept);
				warm.erase(kept);
				remote.SetParked(false);
				remote.SetPaused(false);
				remote.Show();
				attached = true;
				logger::info("portal: {} was kept behind its door{}", Probe::DescribeCell(candidate->cell), remote.FullyLoaded() ? ", loaded" : ", still loading");
			} else {
				const Diag::Scope scope("attach");
				attached = remote.Attach(candidate->cell, here->IsInteriorCell() && Mergeable(here, candidate->cell));
			}
			logger::info("portal: attach took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - attachStarted).count());
			if (attached) {
				SetActive(Active{ candidate->door->GetFormID(), candidate->linkedDoor, candidate->cell, remote.Generation() });
				// Decided for this door from the next frame on; kept from the last portal's (an inn's
				// door standing open), a shut door swung open for a moment as its space came.
				doorWanted = false;
				lastEvent = std::format("activated door {:08X} -> {}", candidate->door->GetFormID(), Probe::DescribeCell(candidate->cell));
				// Loaded behind a door, the engine faded the space's references in as they came:
				// the street opened with its market stalls missing for a fifth of a second.
				for (auto* cell : remote.Cells()) {
					NpcTravel::ShowAtOnce(cell, kShowAtOnceSeconds);
				}
				// Indoors, the street's distant LOD is placed for it from now on, not from the merge:
				// the engine's own update at the crossing, centred on the interior, showed the low LOD
				// town and land in the street's houses for the half second the portal's picture was
				// drawn before the merge.
				if (const auto mapping = Doors::TryCompute(*GetActive()); mapping && here->IsInteriorCell() && !candidate->cell->IsInteriorCell()) {
					Portal::StreetLOD::Show(candidate->cell, mapping->toLinked, false);
				}
			} else {
				failedCell = candidate->cell->GetFormID();
				failedCooldown = kRetryCooldown;
				lastEvent = std::format("could not attach {}", Probe::DescribeCell(candidate->cell));
			}
			logger::info("portal: {}", lastEvent);
		}
		UpdateWarm(here, a_delta);
		std::vector<MergedCull::Room> culls;
		if (const auto portal = GetActive()) {
			remote.Update(a_delta);
			// Also while the door is still swinging shut after the portal stopped being ready.
			if (const auto mapping = Doors::TryCompute(*portal); mapping && (ActivePortal() || (openDoor.door == portal->sourceDoor && !openSwing.Shut()))) {
				OpenDoor(Lookup(portal->sourceDoor), mapping->inward, a_delta);
			}
			// Loaded from further out than usual (farActivate), the space is only merged where it would
			// have been loaded: the street merged into an interior is drawn whole, seen or not (it
			// casts the sun's shadows, MergedCull), 2.6 ms a frame in Honningbrew Meadery's cellar.
			auto*      shownDoor = Lookup(portal->sourceDoor);
			const bool closeBy = shownDoor && player->GetPosition().GetDistance(shownDoor->GetPosition()) <= NearRadius();
			const bool mergeable = !remote.Merged() && closeBy && remote.Healthy() && remote.FullyLoaded() && Mergeable(here, portal->cell);
			const bool waiting = WaitForPicture(*portal, here, mergeable);
			if (mergeable && !waiting) {
				CloseDoor();
				try {
					Diag::CaptureEventFrames(L"merge", 40);
					const auto mergeStarted = std::chrono::steady_clock::now();
					const auto placement = PlacementOf(*portal, Doors::Compute(*portal));
					const auto toSource = placement.toSource;
					const Diag::Scope scope("merge");
					const auto        result = remote.Merge(placement);
					if (walkIntoRooms && !MergedOutdoors()) {
						MergeRoomCollision(*portal, here->GetbhkWorld());
					}
					if (MergedOutdoors()) {
						MergeLOD(toSource, Lookup(portal->sourceDoor));
					}
					// The room's lights, still in the engine's scene from when it was the player's
					// space (walked out of it just now), into the portal's before the first frame:
					// adopted only in the next Update, the room was drawn once without them.
					remote.Update(0.0f);
					// The merged space's doors that stand open, open in the merge's own frame: the
					// stand-ins ran before it this frame, when those doors weren't in view yet.
					UpdateStandIns(player, here, 0.0f);
					logger::info("portal: merge took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - mergeStarted).count());
					logger::info("portal: merged {} into the world ({})", Probe::DescribeCell(portal->cell), result.dump());
				} catch (const std::exception& e) {
					logger::warn("portal: merge failed: {}", e.what());
				}
			}
			if (remote.Merged()) {
				PublishRooms();
				UpdateMerged(*primaryRoom, remote, *portal, 0, doorWanted, a_delta, culls);
				if (const auto mapping = Doors::TryCompute(*portal); mapping && remote.CollisionMerged()) {
					remote.UpdateStandIns(mapping->toLinked.Invert(), here->GetbhkWorld());
				}
			}
			approachLimit = Approach(player, *portal, a_delta);
		}
		UpdateGateMask(here);
		UpdateBeside(here, a_delta, culls);
		MergedCull::Set(std::move(culls));
		PublishRooms();
		// The hold lasts only as long as Approach asks for it, frame by frame: left on by a path that
		// didn't close the passage (walking off it sideways, a crossing), the player walked on with
		// no gravity.
		if (!std::exchange(passageHeldThisFrame, false)) {
			HoldPassageHeight(player, false);
		}
		{
			// Merged, the room is drawn at the door (RemoteCell::Merge moves its 3D by the inverse
			// door mapping) while its Havok world stays where the cell has it.
			const auto      portal = ReadyPortal();
			RE::NiTransform toPhysics;
			std::optional<LightOcclusion::RoomDoor> roomDoor;
			if (const auto mapping = portal ? Doors::TryCompute(*portal) : std::nullopt) {
				roomDoor = LightOcclusion::RoomDoor{ mapping->toLinked * mapping->sourceCenter, mapping->toLinked.rotate * mapping->inward, mapping->halfWidth, mapping->halfHeight };
				if (remote.Merged()) {
					toPhysics = mapping->toLinked;
				}
			}
			if (portal && remote.Merged()) {
				// Lit from its own light scene by the patched Community Shaders, which the engine
				// doesn't cull or fade for this view.
				LightScene::PrepareForView();
			}
			auto* remoteCell = portal ? remote.Cell() : nullptr;
			LightOcclusion::SetWorld(0, remoteCell ? remoteCell->GetbhkWorld() : nullptr, toPhysics, roomDoor,
				remoteCell && remoteCell->IsInteriorCell());
			// Each room merged beside it in its own Havok world, from its own door.
			for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
				const auto beside = std::ranges::find(besideRooms, slot, &BesideRoom::slot);
				auto*      space = beside != besideRooms.end() ? KeptSpace(beside->cell) : nullptr;
				const auto mapping = space ? Doors::TryCompute(Active{ beside->sourceDoor, beside->linkedDoor, beside->cell, space->Generation() }) : std::nullopt;
				if (!mapping) {
					LightOcclusion::SetWorld(slot, nullptr);
					continue;
				}
				LightOcclusion::SetWorld(slot, beside->cell->GetbhkWorld(), mapping->toLinked,
					LightOcclusion::RoomDoor{ mapping->toLinked * mapping->sourceCenter, mapping->toLinked.rotate * mapping->inward, mapping->halfWidth, mapping->halfHeight }, true);
			}
		}
		SetShownCells(remote.Merged() || ActivePortal().has_value());
		if (const auto portal = ReadyPortal()) {
			NpcTravel::Update(*portal, remote.Cells());
		} else {
			NpcTravel::Reset();
		}
		UpdateImageSpace();
		bool planeSet = false;
		if (const auto portal = ReadyPortal()) {
			if (const auto mapping = Doors::TryCompute(*portal); mapping && (!remote.Merged() || MergedOutdoors())) {
				Portal::CameraLimit::SetPlane(mapping->sourceCenter - mapping->inward * mapping->halfDepth, mapping->inward, mapping->halfWidth);
				// Short of the doorway's picture by the camera's near distance (and a unit beyond
				// where walking in carries the player through, Approach, so that still happens).
				const auto* worldCamera = RE::Main::WorldRootCamera();
				const float nearPlane = worldCamera ? worldCamera->GetRuntimeData2().viewFrustum.fNear : 15.0f;
				const bool  gate = !here->IsInteriorCell() && !portal->cell->IsInteriorCell();
				const float pictureDepth = gate ? -(mapping->halfDepth + 2.0f) : mapping->halfDepth + 2.0f;
				Portal::CameraLimit::SetFirstPersonStop(mapping->sourceCenter, mapping->inward, mapping->halfWidth, pictureDepth - nearPlane - 1.0f);
				planeSet = true;
			}
		}
		if (!planeSet) {
			Portal::CameraLimit::ClearPlane();
		}
		PublishOpenDoors();
	}

	void UpdateImageSpace()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		bool  toned = false;
		const auto portal = ReadyPortal();
		if (const auto mapping = portal && here ? Doors::TryCompute(*portal) : std::nullopt) {
			const auto offset = CameraPosition() - mapping->sourceCenter;
			const float depth = offset.x * mapping->inward.x + offset.y * mapping->inward.y;
			// Only in front of the doorway: by depth alone, anyone level with the door's plane
			// (walking along the street past an open door, or teleported along it) had the whole
			// outdoors drawn with the interior's image space: a white flash.
			const float side = std::abs(offset.x * mapping->inward.y - offset.y * mapping->inward.x);
			const float above = std::abs(offset.z) - mapping->halfHeight;
			const auto  fade = [](float a_beyond) { return 1.0f - std::clamp(a_beyond / 100.0f, 0.0f, 1.0f); };
			// Walking in a merged room, the camera past the doorway's plane is in the room however far
			// to the side or upstairs: there the outdoor image space came back, and the room
			// brightened as soon as one walked along its wall. Only then: the plane runs on along the
			// street, and from anywhere beyond it the whole street took the room's image space.
			const float inRoom = playerInRoom ? std::clamp(depth / 50.0f, 0.0f, 1.0f) : 0.0f;
			const float inDoorway = std::max(fade(side - mapping->halfWidth) * fade(above), inRoom);
			// The source door's "inward" points away from the player's space: into the interior
			// from outside, out of it from inside.
			UpdateTone(here, portal->cell, here->IsInteriorCell() ? -depth : depth, inDoorway);
			toned = true;
		}
		if (!toned) {
			ClearTone();
		}
	}

	bool NoteRoomForSave()
	{
		const auto portal = ReadyPortal();
		if (!portal || !playerInRoom || !remote.CollisionMerged()) {
			roomToSave.reset();
			return false;
		}
		auto*      player = RE::PlayerCharacter::GetSingleton();
		const auto mapping = Doors::TryCompute(*portal);
		if (!mapping) {
			roomToSave.reset();
			return false;
		}
		roomToSave = RoomSave{ portal->sourceDoor, mapping->toLinked * player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 4.0f }, player->GetAngleZ() + mapping->yawDelta };
		// Kept as they are, the room and the passage through its house stay under the player while
		// the save is written. The spaces kept out of sight behind other doors must not go into it.
		ReleaseWarm("saving");
		attachHoldoff = std::max(attachHoldoff, 3.0f);
		logger::info("portal: saving in the room behind {:08X}: noted at ({:.0f},{:.0f},{:.0f}) in it", roomToSave->door, roomToSave->position.x, roomToSave->position.y,
			roomToSave->position.z);
		return true;
	}

	void SaveRoom(SKSE::SerializationInterface* a_interface)
	{
		if (!roomToSave) {
			return;
		}
		const auto room = *std::exchange(roomToSave, std::nullopt);
		if (a_interface->OpenRecord(kRoomRecord, kRoomRecordVersion)) {
			a_interface->WriteRecordData(room.door);
			a_interface->WriteRecordData(room.position.x);
			a_interface->WriteRecordData(room.position.y);
			a_interface->WriteRecordData(room.position.z);
			a_interface->WriteRecordData(room.yaw);
		}
	}

	void LoadRoom(SKSE::SerializationInterface* a_interface)
	{
		roomLoaded.reset();
		std::uint32_t type = 0, version = 0, length = 0;
		while (a_interface->GetNextRecordInfo(type, version, length)) {
			if (type != kRoomRecord || version != kRoomRecordVersion) {
				continue;
			}
			RoomSave   room;
			RE::FormID saved = 0;
			if (a_interface->ReadRecordData(saved) && a_interface->ReadRecordData(room.position.x) && a_interface->ReadRecordData(room.position.y) &&
				a_interface->ReadRecordData(room.position.z) && a_interface->ReadRecordData(room.yaw) && a_interface->ResolveFormID(saved, room.door)) {
				roomLoaded = room;
			}
		}
	}

	void ForgetRoom()
	{
		roomLoaded.reset();
	}

	void EnterRoomAfterLoad()
	{
		const auto room = std::exchange(roomLoaded, std::nullopt);
		auto*      door = room ? RE::TESForm::LookupByID<RE::TESObjectREFR>(room->door) : nullptr;
		if (door) {
			Travel::ThroughDoorTo(door, room->position, room->yaw);
		}
	}

	void EndHoldoff(float a_seconds)
	{
		attachHoldoff = a_seconds;
		// Just arrived: the door behind the player is looked for at once, not with a scan's delay.
		scanTimer = 0.0f;
	}

	const std::vector<RE::TESObjectCELL*>& RemoteCells()
	{
		return remote.Cells();
	}

	std::optional<float> CameraLimit()
	{
		return approachLimit;
	}

	std::optional<Active> ReadyPortal()
	{
		const auto portal = GetActive();
		return portal && remote.Healthy() && remote.FullyLoaded() ? portal : std::nullopt;
	}

	nlohmann::json RemoteRenderPasses(bool a_drop)
	{
		return remote.RenderPasses(a_drop);
	}

	void SetStepExterior(bool a_step)
	{
		Spaces::tuning.stepExterior = a_step;
	}

	bool StepExterior()
	{
		return Spaces::tuning.stepExterior;
	}

	float DoorOpenDistance()
	{
		return doorOpenRadius;
	}

	void SetDoorOpenDistance(float a_distance)
	{
		const auto& reach = Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance);
		doorOpenRadius = std::clamp(a_distance, static_cast<float>(reach.min), static_cast<float>(reach.max));
	}

	std::optional<Active> CurrentPortal()
	{
		return GetActive();
	}

	std::optional<DoorPair> NearestDoorPair(float a_radius)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		if (!here) {
			return std::nullopt;
		}
		std::optional<DoorPair> best;
		float                   bestDistance = 0.0f;
		const auto              origin = player->GetPosition();
		RE::TES::GetSingleton()->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
			if (const auto linked = PortalLink(a_ref, here)) {
				const float distance = origin.GetDistance(a_ref->GetPosition());
				if (!best || distance < bestDistance) {
					best = DoorPair{ a_ref->GetFormID(), linked->GetFormID() };
					bestDistance = distance;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return best;
	}

	std::optional<DoorPair> DoorPairAt(float a_radius)
	{
		const auto portal = GetActive();
		// In the room walked into, the door is the portal's whatever else is nearer through its walls.
		if (portal && playerInRoom) {
			return DoorPair{ portal->sourceDoor, portal->linkedDoor };
		}
		const auto nearby = NearestDoorPair(a_radius);
		if (!portal) {
			return nearby;
		}
		auto*      door = Lookup(portal->sourceDoor);
		auto*      nearest = nearby ? Lookup(nearby->sourceDoor) : nullptr;
		const auto origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
		if (nearest && (!door || origin.GetDistance(nearest->GetPosition()) < origin.GetDistance(door->GetPosition()))) {
			return nearby;
		}
		return DoorPair{ portal->sourceDoor, portal->linkedDoor };
	}

	bool Merged()
	{
		return remote.Merged();
	}

	bool StreetMerged()
	{
		return MergedOutdoors();
	}

	namespace
	{
		bool streetEye = true;  // diagnostics: false = rooms seen from the street drawn as from inside

		// Seen from the street, how far a room is drawn as an eye used to the street sees it rather
		// than as from inside: 0 within a few steps of its doorway, where walking in has to find the
		// room just as it is inside, 1 from across the street. a_inward: into the room.
		float FarWeight(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward)
		{
			constexpr float kNear = 250.0f;
			constexpr float kFar = 700.0f;
			const auto      offset = CameraPosition() - a_center;
			if (!streetEye || offset.x * a_inward.x + offset.y * a_inward.y >= 0.0f) {
				return 0.0f;
			}
			const float t = std::clamp((offset.Length() - kNear) / (kFar - kNear), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// How bright an eye used to the view sees a_door's room, against the room as drawn from
		// inside. Each space is drawn as an eye adapted to it sees it; for an eye that adapts a third
		// of the way (exposure as luminance^-1/3), what is drawn goes as luminance^2/3, and the
		// exposures adapted to the two differ by the square root of their drawn brightnesses' ratio.
		// Adapting half way (the ratio itself) left a dim inn's doorway across the market black.
		// No brighter than from inside: at night, when the street is the darker, the room is left
		// as it is.
		float StreetEyeScale(RE::FormID a_door)
		{
			const auto scene = ::Render::SceneMeter::Brightness();
			const auto room = Portal::Snapshot::RoomBrightness(a_door);
			return scene && room ? std::min(1.0f, std::sqrt(*room / *scene)) : 1.0f;
		}
	}

	float RoomLightScale(RE::FormID a_door)
	{
		auto* door = Lookup(a_door);
		if (!door || !door->Get3D()) {
			return 1.0f;
		}
		const auto  inward = AwayFrom(door, CameraPosition());
		const float away = FarWeight(Portal::Doors::ShownCenter(door), inward);
		return 1.0f + (StreetEyeScale(a_door) - 1.0f) * away;
	}

	json DescribeAdaptation()
	{
		json rooms = json::array();
		const auto describe = [&](const MergedRoom& a_room) {
			if (a_room.door) {
				const auto brightness = Portal::Snapshot::RoomBrightness(a_room.door);
				rooms.push_back({ { "door", std::format("{:08X}", a_room.door) }, { "brightness", brightness ? json(*brightness) : json() },
					{ "lightScale", 1.0f + (StreetEyeScale(a_room.door) - 1.0f) * a_room.daylight }, { "daylight", a_room.daylight } });
			}
		};
		if (remote.Merged()) {
			describe(*primaryRoom);
		}
		for (const auto& beside : besideRooms) {
			describe(*beside.state);
		}
		const auto scene = ::Render::SceneMeter::Brightness();
		return { { "sceneBrightness", scene ? json(*scene) : json() }, { "rooms", rooms }, { "streetEye", streetEye } };
	}

	void SetStreetEye(bool a_on)
	{
		streetEye = a_on;
	}

	RE::FormID PictureWanted()
	{
		const auto portal = GetActive();
		return portal && !remote.Merged() && portal->sourceDoor == pictureWanted ? pictureWanted : 0;
	}

	DoorPrompt Prompt(RE::FormID a_door)
	{
		auto* door = Lookup(a_door);
		auto* linked = door && enabled ? Portal::Doors::UsedPairOf(door) : nullptr;
		return !linked                     ? DoorPrompt::kOpen :
		       Portal::Doors::StandsOpen(door, linked)    ? DoorPrompt::kNone :
		       DoorUse::Used(a_door)       ? DoorPrompt::kClose :
		                                     DoorPrompt::kOpen;
	}

	bool DoorwayOpen()
	{
		return !openSwing.Shut();
	}

	bool ToneBlendOn()
	{
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		return manager && manager->GetRuntimeData().overrideBaseData == &toneBlend;
	}

	std::optional<Active> ActivePortal()
	{
		return remote.Merged() ? std::nullopt : ReadyPortal();
	}

	void SetMergeEnabled(bool a_enabled)
	{
		mergeEnabled = a_enabled;
		if (!a_enabled && remote.Merged()) {
			ReleaseAll("merging turned off");
		}
		logger::info("portal: merging {}", a_enabled ? "on" : "off");
	}

	bool MergeEnabled()
	{
		return mergeEnabled;
	}

	namespace
	{
		// Render-merge: the render backend (the patched Community Shaders, or the native one on the
		// game's own shaders; Render/Backend.h) masks the merged spaces and the player's against each
		// other; it needs each room's doorway every frame, and MergedRoomOf tells it per draw which
		// room a shape is in. What a merged room needs is in its MergedRoom: the shown room's is
		// primaryRoom (slot 0), the rooms merged beside it have theirs in besideRooms.
		bool mergedOwnLighting = true;  // diagnostics: false = lit like the world

		// A cell attached again right after the player left it (walking out through the doorway)
		// fades its references in over a second, so the room the camera is standing in is see-through.
		// It has been on screen all along (it was the current cell), so it is shown at full opacity.
		void SkipFadeIn(const Portal::RemoteCell& a_space)
		{
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = loaded ? loaded->cell3D.get() : nullptr) {
					Engine::ForceFadeIn(cell3D);
				}
			}
		}

		// The door of the portal in use has swung shut (merged: the room's own door), so the
		// portal can be let go without a doorway left open onto nothing.
		bool DoorShut()
		{
			return remote.Merged() ? primaryRoom->swing.Shut() : openSwing.Shut();
		}

		// Some doorways have a second door in the same frame (Riverwood Trader: 00070E69, not a
		// load door); with the doorway open in the picture it would stand in it.
		void HideDoorsInDoorway(MergedRoom& a_room, RE::TESObjectREFR* a_source, const RE::NiPoint3& a_center)
		{
			auto* cell = a_source->GetParentCell();
			auto* world = cell ? cell->GetRuntimeData().worldSpace : nullptr;
			const auto visit = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell) {
					return;
				}
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (a_ref && a_ref != a_source && base && base->Is(RE::FormType::Door) && a_ref->Get3D() &&
						a_ref->GetPosition().GetDistance(a_center) < 120.0f &&
						std::ranges::none_of(a_room.extraDoors, [&](const Doors::OpenState& a_open) { return a_open.door == a_ref->GetFormID(); })) {
						Doors::Open(a_ref, a_room.extraDoors.emplace_back(), { .hide = true });
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};
			visit(cell);
			if (world && world->persistentCell != cell) {
				visit(world->persistentCell);
			}
		}

		void SendMergedLighting(int a_slot, const std::optional<Render::SpaceLight>& a_lighting)
		{
			::Render::Backend::SetMergedLighting(a_slot, a_lighting);
		}

		// The street merged into the interior gets the sun's light and shadows, through Community
		// Shaders' Interior Sun (the engine's sun shadow light, on as for an interior flagged for it).
		void SendInteriorSun(bool a_on)
		{
			::Render::Backend::SetInteriorSun(a_on);
		}

		// The engine's sun shadow light shares the interior's directional light, so its shadows fall
		// the way the interior's light shines (in Belethor's: low, from the north-east). While the
		// street is merged in, the shadow light gets a light of its own pointed like the outdoor sun.
		// The interior's shapes keep the interior's direction; the shader leaves them unshadowed
		// (Community Shaders, SeamlessParams.w).
		RE::NiPointer<RE::NiDirectionalLight> streetSun;
		RE::NiPointer<RE::NiLight>            shadowLightOwn;  // the shadow light's own, to put back
		RE::BSShadowDirectionalLight*         shadowLightSwapped = nullptr;

		void AimSunShadow(const RE::NiPoint3* a_towardSun)
		{
			auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			auto* shadow = world ? world->GetRuntimeData().sunShadowDirLight : nullptr;
			if (!a_towardSun || !shadow) {
				if (shadowLightSwapped && shadowLightSwapped == shadow && shadow->light == streetSun) {
					shadow->light = shadowLightOwn;
				}
				shadowLightSwapped = nullptr;
				shadowLightOwn.reset();
				return;
			}
			if (!streetSun) {
				streetSun.reset(RE::NiDirectionalLight::Create());
			}
			if (shadow->light != streetSun) {
				shadowLightOwn = shadow->light;
				shadowLightSwapped = shadow;
				if (shadowLightOwn) {
					streetSun->GetLightRuntimeData() = shadowLightOwn->GetLightRuntimeData();
				}
				shadow->light = streetSun;
			}
			// A directional light shines along its local +X.
			RE::NiPoint3 along = *a_towardSun * -1.0f;
			along.Unitize();
			RE::NiPoint3 side = along.UnitCross({ 0.0f, 0.0f, 1.0f });
			if (side.Length() < 0.5f) {
				side = { 1.0f, 0.0f, 0.0f };
			}
			const RE::NiPoint3 up = side.UnitCross(along);
			auto&              m = streetSun->local.rotate;
			for (int row = 0; row < 3; ++row) {
				m.entry[row][0] = along[row];
				m.entry[row][1] = side[row];
				m.entry[row][2] = up[row];
			}
			RE::NiUpdateData update;
			streetSun->Update(update);
			// The shadow light keeps its own copy of the direction and takes it from its light only
			// when the engine next updates it: for the first fifth of a second after the merge the
			// street's shadows fell from the sun as it stood before the street was turned to fit the
			// room.
			shadow->GetShadowDirectionalLightRuntimeData().sunVector = along;
		}

		void SendMergedFog(const std::optional<Render::SpaceFog>& a_fog)
		{
			::Render::Backend::SetMergedFog(a_fog);
		}

		void SendMergedDoor(int a_slot, const std::optional<Render::DoorRect>& a_door)
		{
			::Render::Backend::SetMergedDoor(a_slot, a_door);
		}

		bool gateMasked = false;  // room 0's doorway is the city gate's (UpdateGateMask)

		// Before the city behind a gate is there, the player's own worldspace shows through it, its
		// low-detail copy of the city (StandInSwinging). Where the two gates stand apart, that
		// worldspace's side of the gatehouse lies between them: Solitude's walls its passage up. Masked
		// up to the far gate, the copy of the street beyond shows. The nearest such gate, as there is
		// one mask.
		std::optional<float> StandInGatePassage(Render::DoorRect& a_door)
		{
			const auto         eye = RE::PlayerCharacter::GetSingleton()->GetPosition();
			RE::TESObjectREFR* gate = nullptr;
			RE::TESObjectREFR* farGate = nullptr;
			for (const auto& [id, standIn] : standIns) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!standIn.gate || standIn.picture || !door->Get3D() || (gate && eye.GetDistance(door->GetPosition()) >= eye.GetDistance(gate->GetPosition()))) {
					continue;
				}
				gate = door;
				farGate = RE::TESForm::LookupByID<RE::TESObjectREFR>(standIn.linked);
			}
			if (!gate) {
				return std::nullopt;
			}
			const auto  inward = AwayFrom(gate, eye);
			const float depth = GateDepth(gate, farGate->GetFormID(), inward);
			// Close gates: the player's worldspace has its own passage behind them, and masked, the
			// passage's ceiling opened onto the sky.
			if (depth <= kGatesApart) {
				return std::nullopt;
			}
			const auto  center = Doors::Center(gate);
			const auto  size = Doors::Size(gate);
			a_door = Render::DoorRect::Upright(center, inward, size.halfWidth, size.halfHeight, false);
			// A little past the far gate: the gatehouse's wall stands about its plane.
			constexpr float kPastFarGate = 50.0f;
			return depth + kPastFarGate;
		}

		// Through a city gate the portal pass draws the other worldspace, and where it drew nothing
		// (the sky, beyond its few cells) the world's own shows: the two share their coordinates,
		// distant land and sky. Where the two gates stand apart, the world's own side of the
		// gatehouse lies in between, and showed instead (Solitude's: a brick wall where the sky
		// was). The gate is masked like a merged room's doorway, in room 0, which a gate's portal
		// leaves free; the world's shapes as far as the portal pass draws the other side: its cells
		// reach a whole cell past the far gate in every direction (RemoteCell::kNeighbourRadius).
		// Masked only up to the far gate, the world's outer arch around it stood in the sky as an
		// outline, the part of it beyond the far gate's plane.
		void UpdateGateMask(RE::TESObjectCELL* a_here)
		{
			std::optional<float> passage;
			Render::DoorRect     door;
			const auto           portal = ActivePortal();
			if (portal && ::Render::Backend::CanMaskGate() && !a_here->IsInteriorCell() && !portal->cell->IsInteriorCell()) {
				if (const auto mapping = Doors::TryCompute(*portal)) {
					const auto  farGate = mapping->toLinked * mapping->sourceCenter - mapping->sourceCenter;
					const float depth = farGate.x * mapping->inward.x + farGate.y * mapping->inward.y;
					if (depth > ::Render::Doorway::kMargin) {
						constexpr float kCellSize = 4096.0f;
						passage = depth + kCellSize * Portal::RemoteCell::kNeighbourRadius;
						door = Render::DoorRect::Upright(mapping->sourceCenter, mapping->inward, mapping->halfWidth, mapping->halfHeight, false);
					}
				}
			}
			// Room 0 is a merged room's while one is.
			if (!passage && !remote.Merged() && ::Render::Backend::CanMaskGate() && !a_here->IsInteriorCell()) {
				passage = StandInGatePassage(door);
			}
			if (passage) {
				SendMergedDoor(0, door);
				::Render::Backend::SetGatePassage(*passage);
				gateMasked = true;
			} else if (std::exchange(gateMasked, false)) {
				if (!remote.Merged()) {
					SendMergedDoor(0, std::nullopt);
				}
				::Render::Backend::SetGatePassage(0.0f);
			}
		}

		// The shown cells are read by the actor cull (ID 40335, ActorCullHook), which also runs off
		// the main thread.
		std::mutex                            shownCellsLock;
		std::vector<const RE::TESObjectCELL*> shownCells;  // under shownCellsLock
		// The merged rooms by slot, as the renderer asks for them (MergedRoomOf, MergedRoomScene):
		// set again (PublishRooms) whenever a room is merged, handed from one role to the other, or let
		// go. These, each room's shapes and playerShapes are written in the manager's update and read
		// by the renderer, both on the main thread, so without a lock: MergedRoomOf is asked several
		// times for every draw.
		std::array<const MergedRoom*, ::Render::kMaxRooms>   roomsBySlot{};
		std::array<RE::ShadowSceneNode*, ::Render::kMaxRooms> scenesBySlot{};
		// The street is merged into an interior (room 0): the water drawn is the street's
		// (MergedRoomOf).
		bool streetMerged = false;

		// The kept space a room merged beside the shown one lives in.
		Portal::RemoteCell* KeptSpace(const RE::TESObjectCELL* a_cell)
		{
			const auto it = std::ranges::find(warm, a_cell, &Portal::RemoteCell::Cell);
			return it != warm.end() ? &*it : nullptr;
		}

		void PublishRooms()
		{
			roomsBySlot.fill(nullptr);
			scenesBySlot.fill(nullptr);
			streetMerged = MergedOutdoors();
			if (remote.Merged()) {
				roomsBySlot[0] = primaryRoom.get();
				scenesBySlot[0] = remote.Scene();
			}
			for (const auto& beside : besideRooms) {
				if (auto* space = KeptSpace(beside.cell)) {
					roomsBySlot[beside.slot] = beside.state.get();
					scenesBySlot[beside.slot] = space->Scene();
				}
			}
		}

		void SetShownCells(bool a_shown)
		{
			std::scoped_lock lock(shownCellsLock);
			shownCells.clear();
			if (a_shown) {
				shownCells.assign(remote.Cells().begin(), remote.Cells().end());
			}
			// Their people are drawn as the shown room's are (ActorCullHook); a paused room's are off
			// screen, as the engine finds them.
			for (const auto& beside : besideRooms) {
				if (const auto* space = KeptSpace(beside.cell); space && !beside.paused) {
					shownCells.insert(shownCells.end(), space->Cells().begin(), space->Cells().end());
				}
			}
		}
		std::unordered_set<const RE::BSGeometry*> playerShapes;

		// Skinned shapes whose bones are driven by something that doesn't know about the merge (SMP
		// hair and cloth physics keep simulating where the cell used to be) are stretched from the
		// moved actor back to the cell's own coordinates: one long streak across the view. They are
		// hidden while any of their bones is far from the doorway.
		bool HasStrayBones(RE::BSGeometry* a_shape, const RE::NiPoint3& a_near)
		{
			auto*      skin = a_shape->GetGeometryRuntimeData().skinInstance.get();
			const auto count = skin && skin->skinData ? skin->skinData->bones : 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				if (auto* bone = skin->bones[i]; bone && bone->world.translate.GetDistance(a_near) > 4000.0f) {
					return true;
				}
			}
			return false;
		}

		void ShowStrayShapes(MergedRoom& a_room)
		{
			for (auto& shape : a_room.strays) {
				Portal::Pristine::Show(shape.get());
			}
			a_room.strays.clear();
		}

		// a_street: the street merged into an interior, whose distant land and objects belong to it.
		MarkPass StartMarkPass(const Portal::RemoteCell& a_space, bool a_street, const RE::NiPoint3& a_near)
		{
			MarkPass pass;
			pass.doorway = a_near;
			// The distant land and objects belong to the street: seen only through the doorway.
			if (a_street && mergedLOD) {
				pass.roots.push_back({ RE::NiPointer<RE::NiAVObject>(mergedLOD->root.get()), nullptr });
				for (auto& moved : mergedLOD->persistent) {
					pass.roots.push_back({ RE::NiPointer<RE::NiAVObject>(moved.node.get()), nullptr });
				}
			}
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = loaded ? loaded->cell3D.get() : nullptr) {
					pass.roots.push_back({ RE::NiPointer<RE::NiAVObject>(cell3D), cell });
				}
			}
			return pass;
		}

		// True once every root is done; roots are taken until a_budgetMs is spent (one at least).
		bool StepMarkPass(Portal::RemoteCell& a_space, MarkPass& a_pass, double a_budgetMs)
		{
			const auto started = std::chrono::steady_clock::now();
			while (a_pass.next < a_pass.roots.size()) {
				const auto& root = a_pass.roots[a_pass.next++];
				if (root.cell) {
					a_space.UnfixNewBounds(root.cell);
				} else if (mergedLOD) {
					mergedLOD->bounds.Unfix(root.node.get());
				}
				RE::BSVisit::TraverseScenegraphGeometries(root.node.get(), [&](RE::BSGeometry* a_shape) {
					a_pass.shapes.insert(a_shape);
					if (root.cell && !a_shape->GetAppCulled() && HasStrayBones(a_shape, a_pass.doorway)) {
						a_pass.strays.emplace_back(a_shape);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() >= a_budgetMs) {
					break;
				}
			}
			return a_pass.next >= a_pass.roots.size();
		}

		// a_primary: the shown room's (people walking through its doorway belong to it or the world).
		void FinishMarkPass(MergedRoom& a_room, MarkPass& a_pass, bool a_primary, bool a_street)
		{
			auto& shapes = a_pass.shapes;
			ShowStrayShapes(a_room);
			for (auto& shape : a_pass.strays) {
				Portal::Pristine::Hide(shape.get());
			}
			a_room.strays = std::move(a_pass.strays);
			if (a_primary) {
				std::vector<RE::NiAVObject*> withWorld, withRoom;
				Portal::NpcTravel::MergedOverrides(withWorld, withRoom);
				for (auto* object : withWorld) {
					RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_shape) {
						shapes.erase(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
				for (auto* object : withRoom) {
					RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_shape) {
						shapes.insert(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
			}
			std::unordered_set<const RE::BSGeometry*> player;
			auto*                                     pc = RE::PlayerCharacter::GetSingleton();
			for (auto* root : { pc->Get3D(false), pc->Get3D(true) }) {
				if (root) {
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_shape) {
						player.insert(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
			}
			// A merged room stands in the world at its door, and is often bigger than the house around
			// it: it cast the sun's shadow onto the street. It has its own lighting and no part in the
			// world's shadows: its shapes don't cast any while merged. (Leaving their shadow passes out
			// at the shader property, vfunc 0x2B, instead left the room undrawn altogether.)
			// The street merged into an interior keeps its shadows: they are the sun's shadows on it.
			if (!a_street) {
				for (const auto* shape : shapes) {
					auto* property = shape->GetGeometryRuntimeData().shaderProperty.get();
					if (property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows)) {
						property->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
						a_room.unshadowed.emplace_back(property);
					}
				}
			} else {
				for (auto& property : a_room.unshadowed) {
					property->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
				}
				a_room.unshadowed.clear();
			}
			a_room.shapes = std::move(shapes);
			playerShapes = std::move(player);
		}

		// a_mark false: the merge ended, everything marked goes back. True: listed anew at once, in
		// this frame.
		void MarkMergedShapes(MergedRoom& a_room, Portal::RemoteCell* a_space, bool a_primary, bool a_street, bool a_mark, const RE::NiPoint3& a_near = {})
		{
			a_room.markPass.reset();
			if (a_mark && a_space) {
				auto pass = StartMarkPass(*a_space, a_street, a_near);
				StepMarkPass(*a_space, pass, std::numeric_limits<double>::infinity());
				FinishMarkPass(a_room, pass, a_primary, a_street);
				return;
			}
			ShowStrayShapes(a_room);
			for (auto& property : a_room.unshadowed) {
				property->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
			}
			a_room.unshadowed.clear();
			a_room.shapes.clear();
		}

		// One merged room, every frame it is drawn. a_slot: its slot with the render backend, 0 for
		// the shown room (remote), which alone can be walked into and be the street merged into an
		// interior. a_wanted: its door swings towards open. Its cull is added to a_culls.
		void UpdateMerged(MergedRoom& a_room, Portal::RemoteCell& a_space, const Active& a_portal, int a_slot, bool a_wanted, float a_delta,
			std::vector<MergedCull::Room>& a_culls)
		{
			const auto found = Doors::TryCompute(a_portal);
			if (!found) {
				return;
			}
			const auto& mapping = *found;
			auto*       source = Lookup(a_portal.sourceDoor);
			auto*       linked = Lookup(a_portal.linkedDoor);
			const bool primary = a_slot == 0;
			// The room is placed anew, live, when its place no longer follows from this door pair: a
			// door correction changed (the in-game menu), or the room was merged through another of
			// its doors (an inn's kitchen door, then shown through its front door). Left where the
			// first door put it, it stood off the doorway by however much it misfits its house.
			if (const auto placement = PlacementOf(a_portal, mapping); !a_space.Placed().SamePlace(placement)) {
				a_space.Remap(placement);
				if (primary && a_space.CollisionMerged()) {
					MergeRoomCollision(a_portal, RE::PlayerCharacter::GetSingleton()->GetParentCell()->GetbhkWorld());
				}
			}
			// Both door panels out of the way: the doorway is open in the picture. Of the two doors
			// standing in the same place, the one in the space the door opens into (outwards by
			// default, Doors::SwingingDoor) swings open into it; the other one goes. The same from
			// either side, and from before the space behind was there (the stand-in's door).
			const bool street = primary && MergedOutdoors();
			auto*      room = street ? source : linked;
			const auto intoRoom = street ? RE::NiPoint3{ -mapping.inward.x, -mapping.inward.y, 0.0f } : mapping.inward;
			auto*      swung = Doors::SwingingDoor(source, linked) == source->GetFormID() ? source : linked;
			const auto settings = Doors::CorrectionDoor(source, linked)->GetFormID();
			if (a_room.swungDoor && a_room.swungDoor != swung->GetFormID()) {
				Doors::RestoreLeaves(a_room.swungDoor);  // turned round in the menu
			}
			a_room.swungDoor = swung->GetFormID();
			if (a_room.swing.Shut()) {
				TakeOverDoor(a_room.swing, swung->GetFormID());
			}
			Doors::Swing::Reclaim(swung->GetFormID());
			const bool wasShut = a_room.swing.Step(swung, a_wanted, a_delta);  // see OpenDoor
			if (a_wanted && wasShut && Doors::OpenFromAfar(source, linked)) {
				a_room.swing.SnapOpen();
			}
			// Shut, the doorway is a door again: solid, nothing to walk into, and the door standing in it
			// is the player's space's. The merged space is drawn only beyond the doorway's plane, and its
			// door shut on that plane wasn't drawn at all: from inside Belethor's, the street door
			// (the one that swings) left the doorway open on the street. Shut, both stand in one place.
			const bool shut = a_room.swing.Shut();
			auto*      shown = shut ? source : swung;
			Doors::Open(source, a_room.sourceDoor, { .hide = shown != source, .solid = shut });
			Doors::Open(linked, a_room.linkedDoor, { .hide = shown != linked, .solid = shut });
			a_room.swing.Turn(swung, swung, swung == room ? intoRoom : intoRoom * -1.0f, settings);
			if (!std::exchange(a_room.extraScanned, true)) {
				HideDoorsInDoorway(a_room, source, mapping.sourceCenter);
			}
			// At once in the merge's first frame (and after someone walked through the doorway,
			// RefreshMergedShapes); otherwise twice a second, spread over frames (MarkPass).
			if (!a_room.marked) {
				a_room.marked = true;
				a_room.markTimer = 0.5f;
				if (street) {
					MergePersistent();
				}
				const Diag::Scope scope("mark shapes");
				MarkMergedShapes(a_room, &a_space, primary, street, true, mapping.sourceCenter);
			} else if (!a_room.markPass && (a_room.markTimer -= a_delta) <= 0.0f) {
				a_room.markTimer = 0.5f;
				if (street) {
					MergePersistent();
				}
				a_room.markPass = StartMarkPass(a_space, street, mapping.sourceCenter);
			}
			if (a_room.markPass && StepMarkPass(a_space, *a_room.markPass, 0.4)) {
				FinishMarkPass(a_room, *a_room.markPass, primary, street);
				a_room.markPass.reset();
			}
			// Is the camera in the merged room? Behind the door plane alone doesn't say so: so is
			// the ground beside and behind the house, and from there the room was drawn in place
			// of the world. It is in the room when it is behind the plane and either the player
			// is in the doorway's passage (the camera follows them in), or the player is in front
			// of the door and seen by the camera through the doorway (they just walked out). Both at
			// the doorway's height: on the guard house's flat roof by Whiterun's gate, horizontally
			// "in the passage", the room was drawn and the world around it left out.
			// Only the shown room is walked into; the camera is never in one merged beside it.
			bool cameraInside = false;
			if (primary) {
				const auto depthOf = [&](const RE::NiPoint3& a_point) {
					return (a_point.x - mapping.sourceCenter.x) * mapping.inward.x + (a_point.y - mapping.sourceCenter.y) * mapping.inward.y;
				};
				const auto sideOf = [&](const RE::NiPoint3& a_point) {
					return std::abs((a_point.x - mapping.sourceCenter.x) * mapping.inward.y - (a_point.y - mapping.sourceCenter.y) * mapping.inward.x);
				};
				const auto  camera = CameraPosition();
				const auto  player = RE::PlayerCharacter::GetSingleton()->GetPosition();
				const float cameraDepth = depthOf(camera), playerDepth = depthOf(player);
				if (cameraDepth > 0.0f) {
					if (playerDepth >= 0.0f && a_space.CollisionMerged()) {
						// Walking in the room on its copied collision: the camera, kept out of the
						// room's walls by the same collision, is in it.
						cameraInside = true;
					} else if (playerDepth >= 0.0f) {
						cameraInside = sideOf(player) <= mapping.halfWidth + 40.0f && playerDepth <= 320.0f && FeetAtDoorway(player.z, mapping);
					} else {
						// Where the line from the camera to the player passes the door plane: in the
						// doorway's opening, not above it.
						const auto hit = camera + (player - camera) * (cameraDepth / (cameraDepth - playerDepth));
						cameraInside = sideOf(hit) <= mapping.halfWidth + 10.0f && std::abs(hit.z - mapping.sourceCenter.z) <= mapping.halfHeight + 10.0f;
					}
				}
			}
			// The mask's plane lies a little beyond the door's middle (Render::Doorway::kMargin): a panel
			// shut on it was drawn neither as the player's space nor as the merged one. Shut, the doorway
			// is masked from just behind the panel (the player's door stands in it), or the shut panel was
			// cut away with the space behind it, the sky showing. Just opening or about to shut, from
			// just in front of it when the merged space's door is the one swinging, or it showed the
			// street through itself for a tenth of a second (from inside Belethor's). Not further open:
			// the camera, walking through, came to stand beyond the plane before the crossing, and the
			// room wasn't masked at all. (Not masked at all while shut, the renderer took the room for
			// the world's: drawn through the house's walls once the door opened again.)
			constexpr float kNearlyShut = 0.3f;
			const float     clear = mapping.halfDepth + static_cast<float>(::Render::Doorway::kMargin) + 1.0f;
			const float     shift = a_room.swing.Shut() ? clear : swung == linked && a_room.swing.Amount() < kNearlyShut ? -clear : 0.0f;
			const auto      center = mapping.sourceCenter + mapping.inward * shift;
			const auto door = Render::DoorRect::Upright(center, mapping.inward, mapping.halfWidth, mapping.halfHeight, cameraInside);
			SendMergedDoor(a_slot, door);
			{
				MergedCull::Room cull;
				cull.door = door;
				cull.cells = a_space.Cells();
				// The merged space's door stands where the player's space's does: from outside it is
				// the door seen, shut or swinging, through the doorway or not.
				if (auto* roomDoor = linked->Get3D()) {
					cull.keep.emplace_back(roomDoor);
				}
				cull.doorShut = a_room.swing.Shut();
				cull.street = street;
				if (cull.street && mergedLOD) {
					cull.roots.emplace_back(mergedLOD->root.get());
					for (const auto& moved : mergedLOD->persistent) {
						cull.roots.push_back(moved.node);
					}
				}
				a_culls.push_back(std::move(cull));
			}
			if (primary) {
				SendInteriorSun(street);
			}
			// (Not when the remote cells' own update did the same this frame, still loading.)
			if ((a_room.seconds += a_delta) < 3.0f && !a_space.FadeForced()) {
				SkipFadeIn(a_space);
			}
			// The NPCs of a merged room. Their 3D hangs in the cell's actor node, which the scene's
			// cull leaves out (the engine draws the actors of the player's cells itself, one by one;
			// for a merged cell its per-actor pass reports them visible, yet nothing is drawn). Hung
			// directly under the cell's 3D they are culled and drawn like everything else in the
			// room; they go back where they were when the merge ends. The engine's "not visible"
			// flag and drawn bits are overridden as well (see ActorCullHook in Render/Frame.cpp).
			// The engine would hang them back into the actors' node every frame (ActorHangHook keeps
			// it from doing so). Listed once each: listed every time they were hung, the list grew by
			// the room's people every frame (28000 entries after a minute in Whiterun's market).
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				auto* cellNode = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
				cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
					if (auto* object = actor ? actor->Get3D() : nullptr) {
						::Render::Frame::ShowActor(actor);
						if (cellNode && object->parent && object->parent != cellNode && object->parent->parent == cellNode) {
							if (std::ranges::find(a_room.actors, object, [](const auto& a_entry) { return a_entry.first.get(); }) == a_room.actors.end()) {
								a_room.actors.emplace_back(RE::NiPointer<RE::NiAVObject>(object), RE::NiPointer<RE::NiNode>(object->parent));
							}
							object->parent->DetachChild(object);
							cellNode->AttachChild(object, true);
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			ReturnLeavers(a_room, a_space);
			if ((a_room.lightingTimer -= a_delta) <= 0.0f) {
				a_room.lightingTimer = 1.0f;  // the cell's lighting doesn't change, the world's state might be reset
				a_room.lighting = mergedOwnLighting ? ::Render::SpaceLighting::MergedLighting(a_portal.cell, mapping.toLinked.Invert().rotate) : std::nullopt;
			}
			{
				// Seen from across the street, the room is drawn as an eye used to the street sees
				// it, with the street's light falling in through its doorway; from a few steps away
				// as from inside (FarWeight). The street merged into a room is drawn as it is.
				a_room.door = a_portal.sourceDoor;
				const float away = street || cameraInside ? 0.0f : FarWeight(mapping.sourceCenter, mapping.inward);
				a_room.daylight = away;
				{
					// What comes in is the street as seen through the doorway: as bright as the eye
					// finds the view, tinted as the outdoors' light on a surface facing out of it.
					Render::Daylight daylight{};
					const auto       tint = away > 0.0f ? ::Render::SpaceLighting::OutdoorAmbient(mapping.inward * -1.0f) : std::nullopt;
					const auto       view = ::Render::SceneMeter::Brightness();
					const float      luminance = tint ? 0.2126f * tint->red + 0.7152f * tint->green + 0.0722f * tint->blue : 0.0f;
					if (view && luminance > 0.0f) {
						const float scale = *view * away / luminance;
						daylight.radiance = { tint->red * scale, tint->green * scale, tint->blue * scale };
					}
					::Render::Backend::SetMergedDaylight(a_slot, daylight);
				}
				auto lighting = a_room.lighting;
				if (lighting) {
					lighting->scale = 1.0f + (StreetEyeScale(a_portal.sourceDoor) - 1.0f) * away;
					// Outdoors the engine multiplies the sun by the image space's sunlight scale (1.7 on a
					// clear day in Whiterun) before the shaders get it; the weather's colour alone left
					// the street a dull grey with barely visible shadows, next to the bright street
					// outside.
					if (street) {
						const auto* outdoors = OutdoorImageSpace();
						const float scale = outdoors ? outdoors->hdr.sunlightScale : 1.0f;
						lighting->sun.red *= scale;
						lighting->sun.green *= scale;
						lighting->sun.blue *= scale;
					}
				}
				SendMergedLighting(a_slot, lighting);
				if (primary) {
					if (street && lighting) {
						AimSunShadow(&lighting->towardSun);
					} else {
						AimSunShadow(nullptr);
					}
					// The merged space is fogged as its own: seen from the street, the hall of a keep a
					// few thousand units deep stood in the street's haze, bluish, where the doorway's
					// picture before the merge (and the hall once walked in) had the keep's own. One fog
					// for all merged rooms (the shaders take one): the shown room's.
					const auto fog = street ? ::Render::SpaceLighting::MergedFog() : ::Render::SpaceLighting::CellFog(a_portal.cell);
					SendMergedFog(fog);
					// Up to the doorway it is the street's air.
					const auto front = street || !streetEye ? std::nullopt : ::Render::SpaceLighting::MergedFog();
					::Render::Backend::SetFrontFog(front);
				}
			}
		}

		// A merged room no longer drawn (let go, or no longer among those shown): everything its
		// drawing changed goes back. Its cells stay merged until released (RemoteCell::Release).
		void EndMerged(MergedRoom& a_room, int a_slot)
		{
			for (auto& [object, holder] : a_room.actors) {
				// Only if it is still where it was put (the engine may have reloaded or moved it).
				if (object->parent && holder->parent == object->parent) {
					object->parent->DetachChild(object.get());
					holder->AttachChild(object.get(), true);
				}
			}
			a_room.actors.clear();
			if (a_slot == 0) {
				UnmergeLOD();
				SendInteriorSun(false);
				SendMergedFog(std::nullopt);
				::Render::Backend::SetFrontFog(std::nullopt);
				AimSunShadow(nullptr);
			}
			// Set again by the next update for the rooms still merged.
			MergedCull::Set({});
			SendMergedDoor(a_slot, std::nullopt);
			SendMergedLighting(a_slot, std::nullopt);
			::Render::Backend::SetMergedDaylight(a_slot, std::nullopt);
			a_room.door = 0;
			a_room.daylight = 0.0f;
			a_room.lightingTimer = 0.0f;
			a_room.seconds = 0.0f;
			MarkMergedShapes(a_room, nullptr, a_slot == 0, false, false);
			// The room's door, the one swung (UpdateMerged), left where it is to be taken over.
			for (const auto* state : { &a_room.sourceDoor, &a_room.linkedDoor }) {
				if (state->door && !state->hidden) {
					a_room.swing.LetGo(state->door);
				}
			}
			a_room.swing.Reset();
			a_room.swungDoor = 0;
			Doors::Close(a_room.sourceDoor);
			Doors::Close(a_room.linkedDoor);
			for (auto& open : a_room.extraDoors) {
				Doors::Close(open);
			}
			a_room.extraDoors.clear();
			a_room.extraScanned = false;
			a_room.markTimer = 0.0f;
			a_room.marked = false;
		}

		// ---- rooms merged beside the shown one ([Portals] iShownRooms) -----------------------------
		// Outdoors, the rooms kept behind other doors (warm) whose door is near enough to open are
		// merged too, each drawn in its own doorway and lit by its own light (render backend slots
		// 1..3). View only: the shown room alone is walked into, carries the camera limit, the
		// passage through its house and the image-space blend. Walking up to one of them makes it the
		// shown room without merging it again; the shown room left for a nearer door goes on beside it.

		int BesideWanted()
		{
			return std::max(0, std::min(shownRoomsSetting, ::Render::Backend::MergedRooms()) - 1);
		}

		int FreeSlot()
		{
			for (int slot = 1; slot < ::Render::Backend::MergedRooms(); ++slot) {
				if (std::ranges::none_of(besideRooms, [&](const BesideRoom& a_room) { return a_room.slot == slot; })) {
					return slot;
				}
			}
			return 0;
		}

		// Whether a line of sight from the camera, as it is now, could pass the room's doorway: the
		// doorway on screen or near it, and the camera in front of the door (beside or behind the
		// house none does). Walls in between aren't considered.
		bool DoorwaySeen(const BesideRoom& a_room, const Portal::RemoteCell& a_space)
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return true;
			}
			const auto mapping = Doors::TryCompute(Active{ a_room.sourceDoor, a_room.linkedDoor, a_room.cell, a_space.Generation() });
			if (!mapping) {
				return true;
			}
			const auto      offset = camera->world.translate - mapping->sourceCenter;
			constexpr float kBehindSlack = 10.0f;
			return offset.x * mapping->inward.x + offset.y * mapping->inward.y < kBehindSlack && DoorwayInView(*mapping, kSeenMargin);
		}

		// Paused, the room keeps everything it is drawn with (doors swung, shapes listed, its
		// doorway and light with the renderer), so it is drawn again the moment it wakes. Its cells
		// are parked: the engine's cull doesn't walk them.
		void SetBesidePaused(BesideRoom& a_room, Portal::RemoteCell& a_space, bool a_paused)
		{
			a_room.paused = a_paused;
			a_space.SetParked(a_paused);
			a_space.SetPaused(a_paused);
			logger::info("portal: {} {} (doorway {})", Probe::DescribeCell(a_room.cell), a_paused ? "paused" : "woken", a_paused ? "out of sight" : "in sight");
		}

		// Not drawn any more: its door let go (a stand-in takes it over, picture and all), its cells
		// left out of the cull until it is shown again or let go.
		std::vector<BesideRoom>::iterator HideBeside(std::vector<BesideRoom>::iterator a_room, std::string_view a_why)
		{
			auto* space = KeptSpace(a_room->cell);
			logger::info("portal: {} no longer shown beside the nearest door ({})", Probe::DescribeCell(a_room->cell), a_why);
			EndMerged(*a_room->state, a_room->slot);
			if (space) {
				space->SetParked(true);
			}
			// Destroyed only once the renderer can't find it any more.
			auto dying = std::move(a_room->state);
			const auto next = besideRooms.erase(a_room);
			PublishRooms();
			return next;
		}

		// The space a merged-beside room lives in is let go, or becomes the shown one.
		void HideBesideOf(const RE::TESObjectCELL* a_cell, std::string_view a_why)
		{
			if (const auto it = std::ranges::find(besideRooms, a_cell, &BesideRoom::cell); it != besideRooms.end()) {
				HideBeside(it, a_why);
			}
		}

		// a_cell, kept, becomes the shown room: if it was merged beside, its drawing state goes with
		// it, so it is drawn on without a break.
		void TakeBeside(const RE::TESObjectCELL* a_cell)
		{
			const auto it = std::ranges::find(besideRooms, a_cell, &BesideRoom::cell);
			if (it == besideRooms.end()) {
				return;
			}
			SendMergedDoor(it->slot, std::nullopt);
			SendMergedLighting(it->slot, std::nullopt);
			auto* cell = it->cell;
			auto  previous = std::exchange(primaryRoom, std::move(it->state));
			besideRooms.erase(it);
			PublishRooms();
			logger::info("portal: {} goes on as the shown room", Probe::DescribeCell(cell));
		}

		// The shown room, merged, left for a nearer door: kept and drawn on beside the next one
		// instead of let go. False when it can't be (walked into, the street, no room for it).
		bool DemoteShown(RE::TESObjectCELL* a_here, const Active& a_portal)
		{
			if (BesideWanted() <= 0 || !remote.Merged() || MergedOutdoors() || a_here->IsInteriorCell() || !a_portal.cell || !a_portal.cell->IsInteriorCell() ||
				remote.CollisionMerged() || playerInRoom || !remote.FullyLoaded()) {
				return false;
			}
			if (!FreeSlot()) {
				// Every slot taken: the farthest of them makes room (the shown room's door was the nearest).
				const auto origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
				const auto farthest = std::ranges::max_element(besideRooms, {}, [&](const BesideRoom& a_room) {
					auto* door = Lookup(a_room.sourceDoor);
					return door ? origin.GetDistance(door->GetPosition()) : FLT_MAX;
				});
				HideBeside(farthest, "the shown room goes beside");
			}
			const int slot = FreeSlot();
			if (!slot) {
				return false;
			}
			CloseDoor();
			ClosePassage();
			SetActive(std::nullopt);
			SendMergedDoor(0, std::nullopt);
			SendMergedLighting(0, std::nullopt);
			Portal::RemoteCell space;
			std::swap(space, remote);
			warm.push_back(std::move(space));
			besideRooms.push_back({ a_portal.cell, a_portal.sourceDoor, a_portal.linkedDoor, slot, std::exchange(primaryRoom, std::make_unique<MergedRoom>()) });
			PublishRooms();
			lastEvent = std::format("{} goes on beside the nearer door", Probe::DescribeCell(a_portal.cell));
			logger::info("portal: {}", lastEvent);
			return true;
		}

		void UpdateBeside(RE::TESObjectCELL* a_here, float a_delta, std::vector<MergedCull::Room>& a_culls)
		{
			struct Pick
			{
				Portal::RemoteCell* space;
				RE::FormID          door;
				RE::FormID          linked;
				float               distance;
			};
			std::vector<Pick> picks;
			const int         wanted = MergeSupported() && !a_here->IsInteriorCell() ? BesideWanted() : 0;
			const auto        origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
			for (auto& space : warm) {
				auto* cell = space.Cell();
				if (wanted <= 0 || !cell ||!cell->IsInteriorCell() || !space.Healthy() || !space.FullyLoaded() || !Mergeable(a_here, cell)) {
					continue;
				}
				// By the door nearest the player into it: one doorway per room, its 3D can be in one place.
				const auto target = std::ranges::find(warmTargets, cell->GetFormID(), &WarmTarget::cell);
				auto*      door = target != warmTargets.end() ? Lookup(target->door) : nullptr;
				auto*      linked = target != warmTargets.end() ? Lookup(target->linked) : nullptr;
				if (!door || !linked || !door->Get3D() || !linked->Get3D() || (door->IsLocked() && DoorUse::GetForce() != DoorUse::Force::kAllOpen)) {
					continue;
				}
				// Drawn while its door is to be open (Wanted), and on while it swings shut.
				const auto  shown = std::ranges::find_if(besideRooms, [&](const BesideRoom& a_room) { return a_room.cell == cell && a_room.sourceDoor == door->GetFormID(); });
				const float distance = origin.GetDistance(door->GetPosition());
				if (!Portal::Doors::Wanted(door, linked) && (shown == besideRooms.end() || shown->state->swing.Shut())) {
					continue;
				}
				picks.push_back({ &space, door->GetFormID(), linked->GetFormID(), distance });
			}
			std::ranges::sort(picks, {}, &Pick::distance);
			if (std::cmp_greater(picks.size(), wanted)) {
				picks.resize(wanted);
			}
			for (auto it = besideRooms.begin(); it != besideRooms.end();) {
				const bool picked = std::ranges::any_of(picks, [&](const Pick& a_pick) { return a_pick.space->Cell() == it->cell && a_pick.door == it->sourceDoor; });
				it = picked ? std::next(it) : HideBeside(it, wanted > 0 ? "door out of reach or others nearer" : "none to be shown");
			}
			// One merge a frame at most: each is a long frame.
			bool mergedOne = false;
			for (const auto& pick : picks) {
				auto* cell = pick.space->Cell();
				if (std::ranges::any_of(besideRooms, [&](const BesideRoom& a_room) { return a_room.cell == cell; })) {
					continue;
				}
				const int slot = FreeSlot();
				if (!slot) {
					break;
				}
				const Active portal{ pick.door, pick.linked, cell, pick.space->Generation() };
				auto         state = std::make_unique<MergedRoom>();
				try {
					// Paused as a room kept out of sight: the merge's update below takes its lights in.
					pick.space->SetPaused(false);
					if (!pick.space->Merged()) {
						if (mergedOne) {
							continue;
						}
						mergedOne = true;
						const auto started = std::chrono::steady_clock::now();
						const auto result = pick.space->Merge(PlacementOf(portal, Doors::Compute(portal)));
						pick.space->Update(0.0f);
						logger::info("portal: merged {} beside the nearest door ({}, {:.1f} ms)", Probe::DescribeCell(cell), result.dump(),
							std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
					} else {
						pick.space->SetParked(false);
						logger::info("portal: {} shown beside the nearest door again", Probe::DescribeCell(cell));
					}
				} catch (const std::exception& e) {
					logger::warn("portal: could not merge {} beside the nearest door: {}", Probe::DescribeCell(cell), e.what());
					continue;
				}
				besideRooms.push_back({ cell, pick.door, pick.linked, slot, std::move(state) });
				PublishRooms();
			}
			for (auto& beside : besideRooms) {
				auto* space = KeptSpace(beside.cell);
				if (!space) {
					continue;
				}
				beside.unseen = beside.seen ? 0.0f : beside.unseen + a_delta;
				if (const bool pause = pauseUnseen && beside.unseen >= kPauseAfterSeconds; pause != beside.paused) {
					SetBesidePaused(beside, *space, pause);
				}
				if (beside.paused) {
					ReturnLeavers(*beside.state, *space);
					continue;
				}
				auto* door = Lookup(beside.sourceDoor);
				auto* linked = Lookup(beside.linkedDoor);
				const bool open = door && linked && Portal::Doors::Wanted(door, linked);
				UpdateMerged(*beside.state, *space, Active{ beside.sourceDoor, beside.linkedDoor, beside.cell, space->Generation() }, beside.slot, open, a_delta, a_culls);
			}
		}
	}

	void SetMergedOwnLighting(bool a_own)
	{
		mergedOwnLighting = a_own;
		primaryRoom->lightingTimer = 0.0f;
		for (auto& beside : besideRooms) {
			beside.state->lightingTimer = 0.0f;
		}
	}

	void RefreshMergedShapes()
	{
		primaryRoom->marked = false;
	}

	int ShownRooms()
	{
		return shownRoomsSetting;
	}

	void SetShownRooms(int a_count)
	{
		shownRoomsSetting = std::clamp(a_count, 1, kMaxShownRooms);
	}

	bool PauseUnseenRooms()
	{
		return pauseUnseen;
	}

	void SetPauseUnseenRooms(bool a_pause)
	{
		pauseUnseen = a_pause;
	}

	std::vector<RE::TESObjectCELL*> MergedCells()
	{
		std::vector<RE::TESObjectCELL*> cells;
		if (remote.Merged()) {
			cells = remote.Cells();
		}
		for (const auto& beside : besideRooms) {
			if (const auto* space = KeptSpace(beside.cell); space && !beside.paused) {
				cells.insert(cells.end(), space->Cells().begin(), space->Cells().end());
			}
		}
		return cells;
	}

	int MergedRoomOf(const RE::BSGeometry* a_shape)
	{
		for (int slot = 0; slot < static_cast<int>(roomsBySlot.size()); ++slot) {
			if (roomsBySlot[slot] && roomsBySlot[slot]->shapes.contains(a_shape)) {
				return slot;
			}
		}
		// Water hangs under the water system, not in its cell's 3D, and only the space the player
		// is in has any: with the street merged into an interior it is the street's, with rooms
		// merged into the street the street's too, the player's space. Masked as the player's, the
		// street's canal running under the houses showed in the Ratway's doorway, over the steps
		// down behind it.
		if (streetMerged && netimmerse_cast<RE::BSWaterShaderProperty*>(a_shape->GetGeometryRuntimeData().shaderProperty.get())) {
			return 0;
		}
		// The weather's rain and snow too: they follow the camera into the interior, and fall only in
		// the street seen through the doorway.
		if (streetMerged) {
			const auto* precip = RE::Sky::GetSingleton()->precip;
			if (a_shape == precip->currentPrecip.get() || a_shape == precip->lastPrecip.get()) {
				return 0;
			}
		}
		return -1;
	}

	RE::ShadowSceneNode* MergedRoomScene(int a_slot)
	{
		return a_slot >= 0 && a_slot < static_cast<int>(scenesBySlot.size()) ? scenesBySlot[a_slot] : nullptr;
	}

	void BeforePlayerView()
	{
		for (auto& beside : besideRooms) {
			auto* space = KeptSpace(beside.cell);
			if (!space) {
				continue;
			}
			// With the camera as this view has it: it moved after the update that paused rooms.
			beside.seen = !pauseUnseen || DoorwaySeen(beside, *space);
			if (beside.paused && beside.seen) {
				// Turned towards it faster than the update noticed: woken for this view already, drawn
				// as it was paused (left out of this frame's cull, so whole), not missing.
				beside.unseen = 0.0f;
				SetBesidePaused(beside, *space, false);
				std::scoped_lock lock(shownCellsLock);
				shownCells.insert(shownCells.end(), space->Cells().begin(), space->Cells().end());
			}
			if (!beside.paused) {
				LightScene::PrepareUnshownForView(space->Scene());
			}
		}
	}

	bool IsPlayerShape(const RE::BSGeometry* a_shape)
	{
		return playerShapes.contains(a_shape);
	}

	bool IsShownRemoteCell(const RE::TESObjectCELL* a_cell)
	{
		std::scoped_lock lock(shownCellsLock);
		return std::ranges::find(shownCells, a_cell) != shownCells.end();
	}

	bool IsMergedShape(const RE::BSGeometry* a_shape)
	{
		return MergedRoomOf(a_shape) >= 0;
	}

	std::vector<DoorwayFill> StandIns()
	{
		std::vector<DoorwayFill> fills;
		for (const auto& [id, standIn] : standIns) {
			auto* door = Lookup(id);
			if (door && door->Get3D() && standIn.picture && (!standIn.handedOver || standIn.fade > 0.0f)) {
				fills.push_back({ id, standIn.outdoors, standIn.gate, standIn.handedOver ? standIn.fade : 1.0f });
			}
		}
		return fills;
	}

	std::optional<DoorwayFill> SnapshotChance()
	{
		// The doorway fully open on the real space for a moment: the room's late lights adopted,
		// the door done swinging.
		static std::optional<std::chrono::steady_clock::time_point> since;
		const auto portal = GetActive();
		const bool open = portal && !standIns.contains(portal->sourceDoor) &&
		                  (remote.Merged() ? primaryRoom->swing.Amount() >= 1.0f : (ActivePortal() && openDoor.door == portal->sourceDoor && openSwing.Amount() >= 1.0f));
		if (!open) {
			since.reset();
			return std::nullopt;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!since) {
			since = now;
		}
		if (now - *since < std::chrono::milliseconds(1500)) {
			return std::nullopt;
		}
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool outdoors = portal->cell && !portal->cell->IsInteriorCell();
		const bool gate = outdoors && here && !here->IsInteriorCell();
		// A city gate only when it shows one (StandInSwinging): a picture nobody sees is megabytes on disk.
		if (gate && !Portal::Doors::PictureFromAfar(Portal::Doors::CorrectionDoor({ portal->sourceDoor, true }, { portal->linkedDoor, true }))) {
			return std::nullopt;
		}
		// Nor the street from inside: shown only live (StreetShownLive).
		if (outdoors && !gate) {
			return std::nullopt;
		}
		return DoorwayFill{ portal->sourceDoor, outdoors, gate, 1.0f };
	}

	json MergedShapesReport(const RE::NiPoint3& a_near, float a_far)
	{
		std::vector<const RE::BSGeometry*> shapes;
		for (const auto* room : roomsBySlot) {
			if (room) {
				for (const auto* shape : room->shapes) {
					shapes.push_back(shape);
				}
			}
		}
		json out = json::array();
		for (auto* shape : shapes) {
			auto* geometry = const_cast<RE::BSGeometry*>(shape);
			const auto& world = geometry->world.translate;
			int farBones = 0, bones = 0;
			if (auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get()) {
				bones = static_cast<int>(skin->skinData ? skin->skinData->bones : 0);
				for (int i = 0; i < bones; ++i) {
					if (auto* bone = skin->bones[i]; bone && bone->world.translate.GetDistance(a_near) > a_far) {
						++farBones;
					}
				}
			}
			const bool isFar = world.GetDistance(a_near) > a_far;
			if (isFar || farBones) {
				auto* top = geometry->parent;
				while (top && top->parent && !top->GetUserData()) {
					top = top->parent;
				}
				auto* ref = top ? top->GetUserData() : nullptr;
				out.push_back(std::format("{} '{}' ref {:08X} pos ({:.0f} {:.0f} {:.0f}) bones {}/{} far", geometry->GetRTTI() ? geometry->GetRTTI()->name : "?", geometry->name.c_str(),
					ref ? ref->GetFormID() : 0, world.x, world.y, world.z, farBones, bones));
			}
		}
		return out;
	}

	json SceneCheck(float a_radius)
	{
		// Things that stand still where their reference stands (no physics, no animation, not doors,
		// which the portal hides or swings): what a merge or a hand-over that went wrong leaves
		// hanging nowhere or in the wrong place.
		const auto still3D = [](RE::TESObjectREFR* a_ref) -> RE::NiAVObject* {
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->As<RE::Actor>() ||
				!base->Is(RE::FormType::Static, RE::FormType::MovableStatic, RE::FormType::Tree, RE::FormType::Flora, RE::FormType::Furniture, RE::FormType::Container) ||
				static_cast<RE::TESObject*>(base)->IsMarker()) {
				return nullptr;
			}
			return a_ref->Get3D();
		};
		// In an interior built of rooms, what lies outside them hangs nowhere and is drawn through
		// the graph of the player's cell (ENGINE.md, Culling).
		auto*      playerLoaded = RE::PlayerCharacter::GetSingleton()->GetParentCell()->GetRuntimeData().loadedData;
		const auto* graph = playerLoaded ? playerLoaded->portalGraph.get() : nullptr;
		const auto inScene = [&](const RE::NiAVObject* a_object) {
			while (a_object->parent) {
				a_object = a_object->parent;
			}
			const auto listed = [&](const auto& a_list) { return std::ranges::any_of(a_list, [&](const auto& a_child) { return a_child.get() == a_object; }); };
			return a_object == RE::Main::WorldRootNode() || (graph && (listed(graph->alwaysRenderChildren) || listed(graph->pendingAlwaysRenderAdds)));
		};
		struct Part
		{
			int                      checked = 0;
			std::vector<std::string> problems;
		};
		// a_toWorld: where the space's own coordinates are drawn.
		const auto checkRef = [&](Part& a_part, RE::TESObjectREFR* a_ref, const RE::NiTransform& a_toWorld) {
			auto* object = still3D(a_ref);
			if (!object) {
				return;
			}
			++a_part.checked;
			const auto expected = a_toWorld * a_ref->GetPosition();
			const auto& at = object->world.translate;
			auto*       model = a_ref->GetBaseObject()->As<RE::TESModel>();
			const auto  name = std::format("{:08X} '{}'", a_ref->GetFormID(), model ? model->GetModel() : "");
			if (!inScene(object)) {
				a_part.problems.push_back(std::format("{} hangs nowhere", name));
			} else if (at.GetDistance(expected) > 50.0f) {
				a_part.problems.push_back(std::format("{} drawn at ({:.0f} {:.0f} {:.0f}), belongs at ({:.0f} {:.0f} {:.0f})", name, at.x, at.y, at.z, expected.x, expected.y, expected.z));
			}
		};
		// A door drawn without its leaves (Pristine): shapes hidden that no opener hid.
		const auto checkDoor = [&](Part& a_part, RE::TESObjectREFR* a_ref) {
			auto* base = a_ref->GetBaseObject();
			if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDisabled() || !a_ref->Get3D() || static_cast<RE::TESObject*>(base)->IsMarker()) {
				return;
			}
			++a_part.checked;
			if (const auto hidden = Doors::HiddenByNoOpener(a_ref)) {
				a_part.problems.push_back(std::format("door {:08X}: {} shape(s) hidden by no opener", a_ref->GetFormID(), hidden));
			}
		};
		const auto checkCellRefs = [&](Part& a_part, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_center, const RE::NiTransform& a_toWorld) {
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref->GetPosition().GetDistance(a_center) <= a_radius) {
					checkRef(a_part, a_ref, a_toWorld);
					checkDoor(a_part, a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};
		const auto partJson = [](const Part& a_part) {
			const auto shown = std::min<std::size_t>(a_part.problems.size(), 40);
			return json{ { "checked", a_part.checked }, { "problemCount", a_part.problems.size() },
				{ "problems", std::vector<std::string>(a_part.problems.begin(), a_part.problems.begin() + shown) } };
		};

		// The player's own space, drawn where it stands.
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      here = player->GetParentCell();
		const auto center = player->GetPosition();
		Part       own;
		std::vector<RE::TESObjectCELL*> ownCells{ here };
		if (auto* tes = RE::TES::GetSingleton(); !here->IsInteriorCell() && tes->gridCells) {
			ownCells.clear();
			for (std::uint32_t x = 0; x < tes->gridCells->length; ++x) {
				for (std::uint32_t y = 0; y < tes->gridCells->length; ++y) {
					if (auto* cell = tes->gridCells->GetCell(x, y); cell && cell->IsAttached()) {
						ownCells.push_back(cell);
					}
				}
			}
			if (auto* world = here->GetRuntimeData().worldSpace; world && world->persistentCell) {
				ownCells.push_back(world->persistentCell);
			}
		}
		for (auto* cell : ownCells) {
			checkCellRefs(own, cell, center, RE::NiTransform());
		}
		json result{ { "cell", Probe::DescribeCell(here) }, { "here", partJson(own) }, { "merged", nullptr } };
		bool ok = own.problems.empty();

		// The space merged behind the door, drawn moved to the door.
		const auto portal = GetActive();
		if (portal && remote.Merged()) {
			const auto toSource = PlacementOf(*portal, Doors::Compute(*portal)).toSource;
			auto*      linked = Lookup(portal->linkedDoor);
			const auto linkedAt = linked->GetPosition();
			const bool street = !remote.Cell()->IsInteriorCell();
			Part       merged;
			for (auto* cell : remote.Cells()) {
				checkCellRefs(merged, cell, linkedAt, toSource);
			}
			if (auto* world = street ? remote.Cell()->GetRuntimeData().worldSpace : nullptr; world && world->persistentCell) {
				checkCellRefs(merged, world->persistentCell, linkedAt, toSource);
			}
			if (street) {
				// The land of each cell, by where its quads are drawn (horizontally: the move turns
				// only about the vertical).
				for (auto* cell : remote.Cells()) {
					auto* land = cell->GetRuntimeData().cellLand;
					auto* data = land ? land->loadedData : nullptr;
					auto* coordinates = cell->GetCoordinates();
					const auto cellCenter = toSource * RE::NiPoint3{ (coordinates->cellX + 0.5f) * 4096.0f, (coordinates->cellY + 0.5f) * 4096.0f, 0.0f };
					for (int quad = 0; data && quad < 4; ++quad) {
						auto* mesh = data->mesh[quad];
						if (!mesh) {
							continue;
						}
						++merged.checked;
						const auto& c = mesh->worldBound.center;
						if (!inScene(mesh)) {
							merged.problems.push_back(std::format("land of {:08X} quad {} hangs nowhere", cell->GetFormID(), quad));
						} else if (std::hypot(c.x - cellCenter.x, c.y - cellCenter.y) > 3000.0f) {
							merged.problems.push_back(std::format("land of {:08X} quad {} drawn around ({:.0f} {:.0f}), belongs around ({:.0f} {:.0f})", cell->GetFormID(), quad, c.x, c.y, cellCenter.x, cellCenter.y));
						}
					}
				}
				if (!mergedLOD) {
					merged.problems.push_back("the distant LOD isn't shown with the street");
				} else if (mergedLOD->root->world.translate.GetDistance(toSource.translate) > 1.0f) {
					const auto& at = mergedLOD->root->world.translate;
					merged.problems.push_back(std::format("the distant LOD drawn at ({:.0f} {:.0f} {:.0f}), belongs at ({:.0f} {:.0f} {:.0f})", at.x, at.y, at.z, toSource.translate.x,
						toSource.translate.y, toSource.translate.z));
				}
			}
			ok &= merged.problems.empty();
			result["merged"] = partJson(merged);
			result["merged"]["space"] = Probe::DescribeCell(remote.Cell());
			result["merged"]["street"] = street;
		}
		result["ok"] = ok;
		return result;
	}

	json MergeTest()
	{
		const auto portal = GetActive();
		if (!portal) {
			throw std::runtime_error("no active portal");
		}
		CloseDoor();
		return remote.Merge(PlacementOf(*portal, Doors::Compute(*portal)));
	}

	json Status()
	{
		const auto portal = GetActive();
		return {
			{ "enabled", enabled },
			{ "merged", remote.Merged() },
			{ "mergeEnabled", mergeEnabled },
			{ "doorUse", Portal::DoorUse::Status() },
			{ "standIns", [] {
				 json out = json::array();
				 for (const auto& [id, standIn] : standIns) {
					 out.push_back({ { "door", std::format("{:08X}", id) }, { "swinging", std::format("{:08X}", standIn.swinging) }, { "swing", standIn.swing.Amount() },
						 { "fade", standIn.fade }, { "handedOver", standIn.handedOver }, { "outdoors", standIn.outdoors }, { "cameThrough", standIn.cameThrough } });
				 }
				 return out;
			 }() },
			{ "doorwayPictures", Portal::Snapshot::Describe() },
			{ "remoteStepping", RemoteCell::SteppingEnabled() },
			{ "active", portal ? json{ { "sourceDoor", std::format("{:08X}", portal->sourceDoor) }, { "linkedDoor", std::format("{:08X}", portal->linkedDoor) }, { "mapping", [&] {
										 try {
											 const auto m = Portal::Doors::Compute(*portal);
											 return json{ { "center", { m.sourceCenter.x, m.sourceCenter.y, m.sourceCenter.z } }, { "inward", { m.inward.x, m.inward.y } },
												 { "halfWidth", m.halfWidth }, { "halfDepth", m.halfDepth }, { "yawDelta", m.yawDelta } };
										 } catch (const std::exception& e) {
											 return json(e.what());
										 }
									 }() } }
								: json(nullptr) },
			{ "remote", remote.Describe() },
			{ "lod", [] {
				 const auto count = [](RE::NiNode* a_node) {
					 int shapes = 0;
					 if (a_node) {
						 RE::BSVisit::TraverseScenegraphGeometries(a_node, [&](RE::BSGeometry*) {
							 ++shapes;
							 return RE::BSVisit::BSVisitControl::kContinue;
						 });
					 }
					 return a_node ? shapes : -1;
				 };
				 const auto* active = RE::BGSTerrainManager::GetActiveManager();
				 auto* tes = RE::TES::GetSingleton();
				 return json{ { "active", active && active->worldSpace ? active->worldSpace->GetFormEditorID() : "" }, { "grid", { tes->currentGridX, tes->currentGridY } },
					 { "staticDataLoaded", active && active->staticDataLoaded }, { "land", count(RE::BGSTerrainManager::GetLODLandRoot().get()) },
					 { "objects", count(RE::BGSTerrainManager::GetLODObjectRoot().get()) }, { "trees", count(RE::BGSTerrainManager::GetTreeNode().get()) } };
			 }() },
			{ "lastEvent", lastEvent },
			{ "lightScenes", LightScene::DescribeAll(remote.Cell3D()) },
			{ "warm", [] {
				 json out = json::array();
				 for (const auto& space : warm) {
					 out.push_back({ { "cell", Probe::DescribeCell(space.Cell()) }, { "healthy", space.Healthy() }, { "fullyLoaded", space.FullyLoaded() },
						 { "lights", space.AdoptedLights() }, { "paused", space.Paused() } });
				 }
				 return json{ { "max", shownRoomsSetting }, { "distance", kWarmDistance }, { "sparse", sparse }, { "reach", warmReach }, { "activate", ActivateRadius() },
					 { "spaces", out }, { "targets", warmTargets.size() } };
			 }() },
			{ "shownRooms", [] {
				 json beside = json::array();
				 for (const auto& room : besideRooms) {
					 const auto* space = KeptSpace(room.cell);
					 beside.push_back({ { "cell", Probe::DescribeCell(room.cell) }, { "door", std::format("{:08X}", room.sourceDoor) }, { "slot", room.slot },
						 { "swing", room.state->swing.Amount() }, { "shapes", room.state->shapes.size() }, { "seen", room.seen }, { "paused", room.paused },
						 { "lightScene", space ? LightScene::Describe(space->Scene(), space->Cell3D()) : json(nullptr) } });
				 }
				 json parked = json::array();
				 for (const auto& space : warm) {
					 if (space.Parked()) {
						 parked.push_back(Probe::DescribeCell(space.Cell()));
					 }
				 }
				 return json{ { "setting", shownRoomsSetting }, { "pauseUnseen", pauseUnseen }, { "beside", beside }, { "parked", parked },
					 { "backendRooms", ::Render::Backend::MergedRooms() } };
			 }() },
			{ "lightOcclusion", LightOcclusion::Describe() },
			{ "mergedCull", MergedCull::Describe() },
			{ "render", ::Render::Backend::Describe() },
			{ "npcTravel", NpcTravel::Status() },
			// Which Havok world the engine steps (*ID 400440) against the player's and the remote one.
			{ "havok", [] {
				 static REL::Relocation<RE::bhkWorld**> current{ REL::RelocationID(514280, 400440) };
				 auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				 const auto ptr = [](const void* a_p) { return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_p)); };
				 return json{ { "stepped", ptr(*current) }, { "playerCell", ptr(here ? here->GetbhkWorld() : nullptr) },
					 { "remote", ptr(remote.Cell() ? remote.Cell()->GetbhkWorld() : nullptr) } };
			 }() },
		};
	}
}

namespace
{
	std::vector<Portal::RemoteCell>::iterator LetGoKept(std::vector<Portal::RemoteCell>::iterator a_space, std::string_view a_why)
	{
		Portal::Manager::HideBesideOf(a_space->Cell(), a_why);
		logger::info("portal: let go of {}, kept behind its door ({})", Probe::DescribeCell(a_space->Cell()), a_why);
		a_space->Release();
		return warm.erase(a_space);
	}
}
