#include "Skyrim/Portal/Pristine.h"

namespace
{
	enum class Kind
	{
		kHidden,
		kMoved,
		kTwoSided,
		kLayer,
		kFlags,
	};

	struct Change
	{
		RE::NiObject*   object;
		Kind            kind;
		RE::NiTransform before;  // kMoved
		RE::COL_LAYER   layer;   // kLayer: before
		std::uint32_t   flags;   // kFlags: before
	};

	using CreateClone = RE::NiObject* (*)(RE::NiObject*, RE::NiCloningProcess&);
	struct Hooked
	{
		std::uintptr_t vtable;
		CreateClone    original;
	};

	// Read by the copy hook on the engine's loading threads.
	std::mutex          lock;
	std::vector<Change> changes;
	std::vector<Hooked> hooked;  // a vtable per class of changed object: a handful

	std::uintptr_t VtableOf(const RE::NiObject* a_object)
	{
		return *reinterpret_cast<const std::uintptr_t*>(a_object);
	}

	RE::hkpRigidBody* HavokBodyOf(RE::NiObject* a_body)
	{
		return static_cast<RE::hkpRigidBody*>(static_cast<RE::bhkRigidBody*>(a_body)->referencedObject.get());
	}

	// A few of each kind: the first copies of a model come in dozens of shapes at once.
	std::array<std::atomic<int>, 5> repairsLogged{};

	// NiObject::CreateClone (vfunc 0x17), hooked in the vtable of each class an object was changed
	// in, when the first one is (Record): the copy is made there, its members copied from the
	// original.
	RE::NiObject* CreateCloneThunk(RE::NiObject* a_original, RE::NiCloningProcess& a_cloning)
	{
		CreateClone original = nullptr;
		{
			std::scoped_lock guard(lock);
			original = std::ranges::find(hooked, VtableOf(a_original), &Hooked::vtable)->original;
		}
		auto* copy = original(a_original, a_cloning);
		std::scoped_lock guard(lock);
		const auto it = std::ranges::find(changes, a_original, &Change::object);
		if (it == changes.end()) {
			return copy;
		}
		switch (it->kind) {
		case Kind::kHidden:
			static_cast<RE::NiAVObject*>(copy)->SetAppCulled(false);
			break;
		case Kind::kMoved:
			static_cast<RE::NiAVObject*>(copy)->local = it->before;
			break;
		case Kind::kTwoSided:
			static_cast<RE::BSShaderProperty*>(copy)->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			break;
		case Kind::kLayer:
			HavokBodyOf(copy)->collidable.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(it->layer);
			break;
		case Kind::kFlags:
			*reinterpret_cast<std::uint32_t*>(&static_cast<RE::bhkNiCollisionObject*>(copy)->flags) = it->flags;
			break;
		}
		if (repairsLogged[std::to_underlying(it->kind)]++ < 3) {
			constexpr std::array<const char*, 5> kKinds{ "shown", "turned back", "one-sided", "collision layer put back", "collision flags put back" };
			logger::info("portal: a copy of '{}' made as before the portal changed it ({})", a_original->GetRTTI()->name, kKinds[std::to_underlying(it->kind)]);
		}
		return copy;
	}

	void Record(RE::NiObject* a_object, Kind a_kind, const RE::NiTransform& a_before = {}, RE::COL_LAYER a_layer = RE::COL_LAYER::kUnidentified, std::uint32_t a_flags = 0)
	{
		const auto vtable = VtableOf(a_object);
		std::scoped_lock guard(lock);
		assert(std::ranges::find(changes, a_object, &Change::object) == changes.end());
		changes.push_back({ a_object, a_kind, a_before, a_layer, a_flags });
		if (!std::ranges::contains(hooked, vtable, &Hooked::vtable)) {
			// Listed before the slot is written: the hook looks its original up there.
			hooked.push_back({ vtable, reinterpret_cast<CreateClone*>(vtable)[0x17] });
			REL::Relocation<std::uintptr_t>(vtable).write_vfunc(0x17, &CreateCloneThunk);
		}
	}

	void Forget(RE::NiObject* a_object)
	{
		std::scoped_lock guard(lock);
		const auto it = std::ranges::find(changes, a_object, &Change::object);
		assert(it != changes.end());
		changes.erase(it);
	}

	RE::NiObject* BodyOf(RE::NiAVObject* a_object)
	{
		return a_object->collisionObject->AsBhkNiCollisionObject()->body.get();
	}
}

namespace Portal::Pristine
{
	void Hide(RE::BSGeometry* a_shape)
	{
		a_shape->SetAppCulled(true);
		Record(a_shape, Kind::kHidden);
	}

	void Show(RE::BSGeometry* a_shape)
	{
		a_shape->SetAppCulled(false);
		Forget(a_shape);
	}

	void Moved(RE::NiAVObject* a_node, const RE::NiTransform& a_before)
	{
		Record(a_node, Kind::kMoved, a_before);
	}

	void MovedBack(RE::NiAVObject* a_node)
	{
		Forget(a_node);
	}

	void TwoSided(RE::BSShaderProperty* a_property)
	{
		a_property->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		Record(a_property, Kind::kTwoSided);
	}

	void OneSided(RE::BSShaderProperty* a_property)
	{
		a_property->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		Forget(a_property);
	}

	void LayerChanged(RE::NiAVObject* a_object, RE::COL_LAYER a_before)
	{
		Record(BodyOf(a_object), Kind::kLayer, {}, a_before);
	}

	void LayerBack(RE::NiAVObject* a_object)
	{
		Forget(BodyOf(a_object));
	}

	void FlagsChanged(RE::bhkNiCollisionObject* a_collision, std::uint32_t a_before)
	{
		Record(a_collision, Kind::kFlags, {}, RE::COL_LAYER::kUnidentified, a_before);
	}

	void FlagsBack(RE::bhkNiCollisionObject* a_collision)
	{
		Forget(a_collision);
	}
}
