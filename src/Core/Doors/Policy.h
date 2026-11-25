#pragma once

// Whether a door pair is to stand open. A door doesn't open as the player comes near. It opens
// when someone uses it and shuts a few seconds after; it stands open for good when it is a city
// gate, set to stand open always (once its doorway has a picture to show from afar: never seen
// open, it showed the plain colour), or every door is to (the global force). Forced shut, only a
// use opens it.
namespace Core::Doors
{
	// The global setting for all doors.
	enum class Force
	{
		kNone = 0,
		kAllOpen = 1,
		kAllShut = 2,
	};

	// What the decision is made from; the caller reads it off the game. `door` is the door in the
	// player's space, `linked` its other end (which needn't be loaded).
	struct PairFacts
	{
		Force force = Force::kNone;
		bool  doorOutdoors = false;
		bool  linkedOutdoors = false;
		bool  alwaysOpen = false;  // the pair's own "always open" switch
		bool  doorUsed = false;    // someone used the door and it is still open
		bool  linkedUsed = false;
	};

	// A locked door stays a door: closed, solid, to be picked or opened with its key. Unless every
	// door is to stand open.
	[[nodiscard]] inline bool LockedShut(bool a_locked, Force a_force)
	{
		return a_locked && a_force != Force::kAllOpen;
	}

	// A door out of a building (a door indoors, its pair outdoors) shows the street only live,
	// never a picture of it: the street's pictures, taken from inside and drawn with their depth,
	// broke up into streaks seen from beside the doorway. So such a door opens from inside only once
	// its street is shown, and has no stand-in.
	[[nodiscard]] inline bool StreetShownLive(bool a_doorOutdoors, bool a_linkedOutdoors)
	{
		return !a_doorOutdoors && a_linkedOutdoors;
	}

	// The pair stands open whoever uses it, and already before the space behind it is there: it is
	// never seen shut, so never seen opening either. a_hasPicture: the doorway has a picture of
	// what is behind it; asked only when the answer matters.
	template <class HasPicture>
	[[nodiscard]] bool OpenFromAfar(const PairFacts& a_facts, HasPicture&& a_hasPicture)
	{
		switch (a_facts.force) {
		case Force::kAllOpen:
			return true;
		case Force::kAllShut:
			return false;
		case Force::kNone:
			break;
		}
		return (a_facts.doorOutdoors && a_facts.linkedOutdoors) ||
		       (!StreetShownLive(a_facts.doorOutdoors, a_facts.linkedOutdoors) && a_facts.alwaysOpen && a_hasPicture());
	}

	// The pair stands open whoever uses it: from afar, or set to stand open always and its space
	// shown. The picture is only taken of a doorway seen open, so a door never seen open from
	// one side would stay shut for good. Such a door was seen shut until its space came, so it
	// swings open then (what stands open from afar is shown open at once).
	template <class HasPicture, class SpaceShown>
	[[nodiscard]] bool StandsOpen(const PairFacts& a_facts, HasPicture&& a_hasPicture, SpaceShown&& a_spaceShown)
	{
		return OpenFromAfar(a_facts, a_hasPicture) || (a_facts.force == Force::kNone && a_facts.alwaysOpen && a_spaceShown());
	}

	// The pair's doors are to be open now: standing open, or used.
	template <class HasPicture, class SpaceShown>
	[[nodiscard]] bool Wanted(const PairFacts& a_facts, HasPicture&& a_hasPicture, SpaceShown&& a_spaceShown)
	{
		return StandsOpen(a_facts, a_hasPicture, a_spaceShown) || a_facts.doorUsed || a_facts.linkedUsed;
	}
}
