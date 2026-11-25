#pragma once

// The engine builds a reference's 3D as a copy of an already loaded one of the same model, all of
// it as it is at that moment (ENGINE.md, Culling). What the portals change in a 3D (door shapes
// hidden, leaves swung, shapes drawn from both sides, collision made walk-through or still) came
// along into every reference of that model loaded after, for good, since whoever changes one puts
// back only what it changed on that one: loading a save inside The Drunken Huntsman hid the leaves
// of every Whiterun dragon door the street loaded next, and Warmaiden's inner doors, loaded while
// the inn's were stilled for its merge, couldn't be used (their bodies stayed where the inn's
// stood). Every such change goes through here, and a copy of a changed object is made as the
// original was before the change.
namespace Portal::Pristine
{
	// Main thread. Each change is recorded once, by the one who made it, and put back by it (the
	// second of each pair); while recorded, copies are made without it.

	// a_shape hidden (it was shown).
	void Hide(RE::BSGeometry* a_shape);
	void Show(RE::BSGeometry* a_shape);

	// a_node's local transform changed from a_before.
	void Moved(RE::NiAVObject* a_node, const RE::NiTransform& a_before);
	void MovedBack(RE::NiAVObject* a_node);

	// a_property drawn from both sides (it wasn't).
	void TwoSided(RE::BSShaderProperty* a_property);
	void OneSided(RE::BSShaderProperty* a_property);

	// a_object's collision layer changed from a_before.
	void LayerChanged(RE::NiAVObject* a_object, RE::COL_LAYER a_before);
	void LayerBack(RE::NiAVObject* a_object);

	// a_collision's flags changed from a_before (made visual only: RemoteCell's stilled objects).
	void FlagsChanged(RE::bhkNiCollisionObject* a_collision, std::uint32_t a_before);
	void FlagsBack(RE::bhkNiCollisionObject* a_collision);
}
