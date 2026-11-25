#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Core/Math/Vec3.h"

// The places of a game and the doorways between them, from a plain list of door records: no engine
// types. The shapes follow the door census (tools/census, schema door-census/1): a space is an
// interior cell or a worldspace, a door is a placed teleport door, and the two doors that lead to
// each other make one doorway. A census file and a scan at data load fill the same records.
//
// Everything is addressed by dense indices (SpaceIx, DoorIx, DoorwayIx) so the scheduler and the
// adapter can keep plain vectors beside the graph; the string ids are kept for lookups and logs.
namespace Core::Graph
{
	using SpaceIx = std::uint32_t;
	using DoorIx = std::uint32_t;
	using DoorwayIx = std::uint32_t;

	inline constexpr SpaceIx kNoSpace = std::numeric_limits<SpaceIx>::max();
	inline constexpr DoorIx kNoDoor = std::numeric_limits<DoorIx>::max();
	inline constexpr DoorwayIx kNoDoorway = std::numeric_limits<DoorwayIx>::max();

	enum class SpaceKind : std::uint8_t
	{
		Interior,
		Worldspace,  // an exterior worldspace, a child one (a walled city's own worldspace) included
	};

	enum class DoorKind : std::uint8_t
	{
		Hinged,
		Gate,   // between two worldspaces, or two parts of one
		Hatch,  // trapdoors and the like
	};

	// How a child worldspace's coordinates relate to its parent's, from where the doors of its gate
	// doorways stand: a walled city placed over its parent's footprint is Shared (the crossing keeps
	// the player's position instead of moving them to a marker).
	enum class Coordinates : std::uint8_t
	{
		NoGates,   // no gate doorway to judge by (or not a child worldspace)
		Separate,  // no gate pair lines up
		Partial,
		Shared,    // every gate pair lines up
	};

	// Gate pairs whose two doors stand within this horizontal distance count as lined up (census default).
	inline constexpr float kSharedThreshold = 1000.0f;

	// The input: what an extractor lists. Ids are strings that stay the same between runs
	// ("<plugin>:<hex>" in the census); a door's `space`, `destDoor` and `destSpace` name other records.
	struct SpaceRecord
	{
		std::string id;
		SpaceKind   kind = SpaceKind::Interior;
		std::string parent;  // a child worldspace's parent worldspace id, else empty
	};

	struct DoorRecord
	{
		std::string id;
		std::string space;  // the space it stands in
		Vec3        pos;
		Vec3        rot;  // radians about x, y, z
		// Half the size of its quad; 0 where it is not known.
		float halfWidth = 0.0f;
		float halfHeight = 0.0f;
		// The door it leads to, and the space behind it (for a destination that is not in the door
		// list); either may be empty. A listed destination door's own space wins.
		std::string destDoor;
		std::string destSpace;
		Vec3        landPos;  // where the player is put on the other side
		Vec3        landRot;
		DoorKind    kind = DoorKind::Hinged;
		bool        locked = false;
		bool        scripted = false;      // the reference has scripts
		bool        baseScripted = false;  // its base object has scripts
		bool        minimalUse = false;
		bool        automatic = false;
		bool        hidden = false;  // initially disabled
		bool        openByDefault = false;
	};

	struct Door
	{
		std::string id;
		SpaceIx     space = kNoSpace;
		DoorwayIx   doorway = kNoDoorway;
		Vec3        pos;
		Vec3        rot;
		float       halfWidth = 0.0f;
		float       halfHeight = 0.0f;
		Vec3        landPos;
		Vec3        landRot;
		DoorKind    kind = DoorKind::Hinged;
		bool        locked = false;
		bool        scripted = false;
		bool        baseScripted = false;
		bool        minimalUse = false;
		bool        automatic = false;
		bool        hidden = false;
		bool        openByDefault = false;
	};

	enum class Link : std::uint8_t
	{
		Paired,  // two doors that lead to each other
		OneWay,  // the door leads to a door that leads elsewhere; that other door is not part of this doorway
		Broken,  // the destination door does not exist (the space behind it may be known)
	};

	// One way through: both ends of a pair of doors, or the single end of a one-way or broken door.
	struct Doorway
	{
		DoorIx   a = kNoDoor;  // the door in spaceA
		DoorIx   b = kNoDoor;  // the door in spaceB, if the link is paired
		SpaceIx  spaceA = kNoSpace;
		SpaceIx  spaceB = kNoSpace;  // kNoSpace if unknown
		DoorKind kind = DoorKind::Hinged;
		Link     link = Link::Paired;
		bool     locked = false;    // either end
		bool     scripted = false;  // either end, reference or base object

		// The space across from a_space (the same one for a doorway inside one space).
		[[nodiscard]] SpaceIx Other(SpaceIx a_space) const { return a_space == spaceA ? spaceB : spaceA; }
		// The door standing in a_space; kNoDoor if the doorway has none there.
		[[nodiscard]] DoorIx DoorIn(SpaceIx a_space) const
		{
			if (a_space == spaceA) {
				return a;
			}
			return a_space == spaceB ? b : kNoDoor;
		}
	};

	struct Space
	{
		std::string            id;
		SpaceKind              kind = SpaceKind::Interior;
		SpaceIx                parent = kNoSpace;
		Coordinates            coordinates = Coordinates::NoGates;
		std::vector<DoorIx>    doors;     // the doors standing in it
		std::vector<DoorwayIx> doorways;  // the doorways with an end in it, each once

		[[nodiscard]] bool CoordinateShared() const { return coordinates == Coordinates::Shared; }
	};

	struct BuildOptions
	{
		float sharedThreshold = kSharedThreshold;
	};

	struct BuildStats
	{
		std::size_t unlistedSpaces = 0;  // named by a door or as a parent but not listed: added (as interiors; parents as worldspaces)
		std::size_t duplicateDoors = 0;  // an id seen again: the later record is ignored
		std::size_t duplicateSpaces = 0;
	};

	struct DoorHit
	{
		DoorIx    door = kNoDoor;
		DoorwayIx doorway = kNoDoorway;
		float     distance = 0.0f;
	};

	class SpaceGraph
	{
	public:
		[[nodiscard]] static SpaceGraph Build(std::span<const SpaceRecord> a_spaces, std::span<const DoorRecord> a_doors, const BuildOptions& a_options = {});

		[[nodiscard]] std::size_t       SpaceCount() const { return spaces.size(); }
		[[nodiscard]] std::size_t       DoorCount() const { return doors.size(); }
		[[nodiscard]] std::size_t       DoorwayCount() const { return doorways.size(); }
		[[nodiscard]] const Space&      GetSpace(SpaceIx a_space) const { return spaces[a_space]; }
		[[nodiscard]] const Door&       GetDoor(DoorIx a_door) const { return doors[a_door]; }
		[[nodiscard]] const Doorway&    GetDoorway(DoorwayIx a_doorway) const { return doorways[a_doorway]; }
		[[nodiscard]] const BuildStats& Stats() const { return stats; }

		[[nodiscard]] SpaceIx FindSpace(std::string_view a_id) const { return Find(spaceIndex, a_id, kNoSpace); }
		[[nodiscard]] DoorIx  FindDoor(std::string_view a_id) const { return Find(doorIndex, a_id, kNoDoor); }

		// The doors standing in a space.
		[[nodiscard]] std::span<const DoorIx> DoorsOf(SpaceIx a_space) const { return spaces[a_space].doors; }
		// The doorways with an end in a space.
		[[nodiscard]] std::span<const DoorwayIx> DoorwaysOf(SpaceIx a_space) const { return spaces[a_space].doorways; }

		// The spaces the doorways of a_space lead to, each once, in index order (not a_space itself).
		[[nodiscard]] std::vector<SpaceIx> Neighbours(SpaceIx a_space) const;

		// The doorways to go through from a_from to a_to, fewest first (breadth first); empty for the
		// same space, nullopt if there is no way. a_skipLocked leaves out doorways with a locked end.
		[[nodiscard]] std::optional<std::vector<DoorwayIx>> Path(SpaceIx a_from, SpaceIx a_to, bool a_skipLocked = false) const;

		// The doors standing in a_space within a_radius (3D) of a_point, nearest first.
		[[nodiscard]] std::vector<DoorHit> DoorsWithin(SpaceIx a_space, const Vec3& a_point, float a_radius) const;

	private:
		struct StringHash
		{
			using is_transparent = void;
			[[nodiscard]] std::size_t operator()(std::string_view a_text) const { return std::hash<std::string_view>{}(a_text); }
		};
		template <class T>
		using Index = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

		template <class T>
		[[nodiscard]] static T Find(const Index<T>& a_index, std::string_view a_id, T a_none)
		{
			const auto it = a_index.find(a_id);
			return it != a_index.end() ? it->second : a_none;
		}

		std::vector<Space>   spaces;
		std::vector<Door>    doors;
		std::vector<Doorway> doorways;
		Index<SpaceIx>       spaceIndex;
		Index<DoorIx>        doorIndex;
		BuildStats           stats;
	};
}
