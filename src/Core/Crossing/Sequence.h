#pragma once

#include <cstdint>
#include <vector>

#include "Core/Graph/SpaceGraph.h"

// One crossing from a space into the next through a doorway, as a state machine driven by plain
// events and answering with actions for the adapter. It carries out nothing itself and decides
// nothing about whether to cross (the player's approach and the scheduler do); it keeps the order
// and the timeouts, and falls back to the engine's own door load rather than hang.
//
//   Idle -Begin-> Approach -Near-> Prewarm -Prewarmed-> Open -DoorOpened-> Commit -AtThreshold-> Cross
//        -Crossed-> Settle -Settled-> Release -Released-> Idle
//
// Abort (the player walked off) before the cross goes back to Idle; Failed, or a timeout while
// waiting on the adapter, goes to Fallback: the engine's own load of that door, then Idle.
namespace Core::Crossing
{
	enum class Phase : std::uint8_t
	{
		Idle,
		Approach,  // walking up to the door; nothing held yet
		Prewarm,   // the space behind is being brought to loaded-and-paused
		Open,      // the door is opening
		Commit,    // ready: waits for the player at the threshold
		Cross,     // the player is moved through
		Settle,    // arrived: the new space is the player's, the cover ends
		Release,   // the space left is let go
		Fallback,  // the engine's own door load is under way
	};

	[[nodiscard]] constexpr const char* Name(Phase a_phase)
	{
		constexpr const char* names[] = { "idle", "approach", "prewarm", "open", "commit", "cross", "settle", "release", "fallback" };
		return names[static_cast<int>(a_phase)];
	}

	enum class EventKind : std::uint8_t
	{
		Begin,        // a doorway is being walked up to (carries the target)
		Near,         // close enough to start preparing
		Prewarmed,    // the adapter has the space behind loaded and paused
		DoorOpened,   // the door finished opening (or was open)
		AtThreshold,  // the player stands in the doorway and means to go through
		Crossed,      // the adapter moved the player into the other space
		Settled,      // the arrival has settled
		Released,     // the adapter let the old space go
		Abort,        // the player walked off or changed their mind
		Failed,       // the adapter could not do what was asked
	};

	struct Target
	{
		Graph::DoorwayIx doorway = Graph::kNoDoorway;
		Graph::DoorIx    door = Graph::kNoDoor;  // the door the player goes through, in `from`
		Graph::SpaceIx   from = Graph::kNoSpace;
		Graph::SpaceIx   to = Graph::kNoSpace;
	};

	struct Event
	{
		EventKind kind = EventKind::Abort;
		Target    target;  // for Begin
	};

	// What covers the moment of crossing.
	enum class Cover : std::uint8_t
	{
		None,
		DoorSwing,   // the door's leaves swing, the picture takes over
		EngineLoad,  // the engine's loading screen
	};

	enum class ActionKind : std::uint8_t
	{
		PrewarmSpace,    // bring `space` to loaded-and-paused
		CancelPrewarm,   // stop preparing `space` (the scheduler may still keep it for its own reasons)
		OpenDoor,        // play the door open
		StartCover,
		Teleport,        // put the player through `door` into `space`, no loading screen
		EndCover,
		ReleaseSpace,    // `space` was left: the scheduler lets it go
		VanillaDoor,     // let the engine take `door` its own way; this doorway is then marked fallback
	};

	struct Action
	{
		ActionKind     kind = ActionKind::EndCover;
		Graph::SpaceIx space = Graph::kNoSpace;
		Graph::DoorIx  door = Graph::kNoDoor;
		Cover          cover = Cover::None;
	};

	struct Timeouts
	{
		// Seconds a phase may wait on the adapter or the player before the crossing gives up.
		// Prewarm and open only count once the player waits in the doorway for the way (the wait
		// before that holds nobody up and is no failure).
		float prewarm = 6.0f;    // then Fallback
		float open = 3.0f;       // then Fallback
		float commit = 15.0f;    // the player lingers at the door: then Idle (not a failure)
		float cross = 2.0f;      // then Fallback
		float settle = 1.0f;     // then on to Release, as if settled
		float release = 2.0f;    // then Idle
		float fallback = 10.0f;  // then Idle
	};

	class Sequence
	{
	public:
		explicit Sequence(const Timeouts& a_timeouts = {}) :
			timeouts(a_timeouts)
		{}

		[[nodiscard]] Phase         GetPhase() const { return phase; }
		[[nodiscard]] bool          Busy() const { return phase != Phase::Idle; }
		[[nodiscard]] const Target& Current() const { return target; }

		// An event; events that don't fit the phase are ignored.
		[[nodiscard]] std::vector<Action> Handle(const Event& a_event);
		// Every frame: the timeouts.
		[[nodiscard]] std::vector<Action> Tick(float a_delta);

	private:
		void Enter(Phase a_phase)
		{
			phase = a_phase;
			elapsed = 0.0f;
		}
		void FallBack(std::vector<Action>& a_out);
		void BeginCross(std::vector<Action>& a_out);

		Timeouts timeouts;
		Phase    phase = Phase::Idle;
		Target   target;
		float    elapsed = 0.0f;
		bool     prewarmAsked = false;
		bool     pendingCross = false;  // the player reached the threshold before the way was ready
	};
}
