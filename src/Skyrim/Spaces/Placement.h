#pragma once

namespace Spaces
{
	// Bounds that don't follow a subtree moved for a merge. Multibound shapes (BSMultiBoundNode, an
	// interior's rooms, a city's house blocks) keep theirs in world coordinates, and nodes flagged
	// kFixedBound (a cell's node, its land and statics nodes) never recompute theirs: moved, they
	// were culled against where they used to be (a room's walls missing seen from inside it, a
	// street's buildings and ground from indoors).
	class MovedBounds
	{
	public:
		// Before a_root is moved (its local transform set, or its parent's): its multibounds as they
		// are, what they go back to.
		void Capture(RE::NiAVObject* a_root);
		// Before a_root is moved and updated: its fixed bounds unfixed, so that the update computes
		// them (one update of the whole cell, not a second one to unfix them after).
		void UnfixBeforeMove(RE::NiAVObject* a_root);
		// After a_root was moved by a_move and updated: bounds recomputed, and the boxes captured
		// that the scene update didn't move along are moved by hand.
		void Move(RE::NiAVObject* a_root, const RE::NiTransform& a_move);
		// While moved: nodes the engine fixed since (land and statics attached as their cell finished
		// loading) are unfixed too. Their bound, fixed where it was computed, stayed in the moved
		// space after the move back: the land in front of the Bannered Mare, seen from the street,
		// was culled against the inn's own coordinates.
		void Unfix(RE::NiAVObject* a_root);
		// After everything moved is back in place and updated with its bounds still unfixed.
		void Restore();
		[[nodiscard]] std::size_t Multibounds() const { return moved.size(); }
		[[nodiscard]] std::size_t Fixed() const { return fixed.size(); }

	private:
		struct Saved
		{
			RE::NiPointer<RE::BSMultiBoundShape> shape;
			RE::NiPoint3                         center;
			RE::NiPoint3                         size;
			RE::NiMatrix3                        rotation;
		};
		std::vector<Saved>                         moved;
		std::size_t                                unmoved = 0;  // moved[unmoved..]: captured, not moved yet
		std::vector<RE::NiPointer<RE::NiAVObject>> fixed;
	};

	// After moving a_root as a whole: each object's previous transform set to the current one, so
	// the motion vectors don't reach back to where it came from.
	void SettleMotion(RE::NiAVObject* a_root);

	// Every bound under a_root computed anew from where its shapes are now, fixed ones included.
	// A scene update leaves them: a city's land, moved into an interior and back, kept the bounds
	// it had there.
	void RecomputeBounds(RE::NiAVObject* a_root);
}
