#pragma once

// One door's swing, held by whoever draws that door open: the portal's door, a merged room's, a
// doorway stand-in's. It runs along the door's own animation curves (SwingAmount) and, when its
// owner lets go (the space was released, the door is handed to another owner), leaves the door
// where it is for a moment so the next owner of the same door takes it over from there.
//
// The room's door is the one swung from both sides of a doorway: from outside as part of the
// merged room, from inside as the portal's door. Walking through hands it from one to the other;
// closing it and swinging it open again from scratch swept it through the view of a player
// standing in the doorway. Several doors can be let go at once: walking out, the room's door is let
// go of by the crossing, and the street door the same frame by the merge taking over from the
// portal. With one slot the second replaced the first, and the room's door, seen from the street,
// swung open again from shut through the camera standing in the doorway.
namespace Portal::Doors
{
	class Swing
	{
	public:
		// 0..1, how far the door has swung open.
		[[nodiscard]] float Amount() const { return amount; }
		[[nodiscard]] bool  Shut() const { return amount <= 0.0f; }

		// Starts from where the door was let go of, if it was just now, else shut. The progress and
		// its direction go together: the progress means a different opening on the opening curve than
		// on the shutting one, and a door taken over half open with the shutting direction jumped
		// back nearly shut and swung open a second time.
		void TakeOver(RE::FormID a_door);
		// One step of progress towards a_wanted (open or shut), timed by a_timed's animations (the
		// swung door, or another that is loaded when the swung one isn't). Turned round half way, the
		// progress is taken from where the other direction's animation is as far open: kept as it
		// was, the door jumped. True when it started out shut.
		bool Step(RE::TESObjectREFR* a_timed, bool a_wanted, float a_delta);
		// Open at once: a door that stands open is never seen opening.
		void SnapOpen() { amount = 1.0f; }
		// Shut and forgotten, once the door is no longer held (Close, or let go of).
		void Reset() { *this = {}; }

		// a_door's leaves turned to where the swing stands: towards a_room, a horizontal world
		// direction, by the pair's settings.
		void Turn(RE::TESObjectREFR* a_timed, RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, RE::FormID a_settings) const;

		// Leaves a_door where it stands for the next owner to take over (TakeOver); shut by itself
		// when nobody does within a few seconds (Expire).
		void LetGo(RE::FormID a_door) const;
		// The owner holds a_door again: not to be put back shut when the let-go expires.
		static void Reclaim(RE::FormID a_door);
		// Main thread, every frame: puts back the doors let go of for good.
		static void Expire(float a_delta);
		// Puts every door let go of back at once.
		static void ForgetAll();

	private:
		float amount = 0.0f;
		bool  opening = false;  // the direction of the progress
	};
}
