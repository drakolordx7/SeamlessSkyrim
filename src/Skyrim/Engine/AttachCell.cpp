#include "Skyrim/Engine/AttachCell.h"

namespace Engine
{
	namespace
	{
		bool attachingRemote = false;

		class AttachingScope
		{
		public:
			AttachingScope() { attachingRemote = true; }
			~AttachingScope() { attachingRemote = false; }
			AttachingScope(const AttachingScope&) = delete;
			AttachingScope& operator=(const AttachingScope&) = delete;
		};
	}

	void AttachCell(RE::TESObjectCELL* a_cell, RE::NiNode* a_parent)
	{
		REL::Relocation<bool (*)(RE::TESObjectCELL*)>               loadTempData{ REL::RelocationID(18637, 19111) };
		REL::Relocation<void (*)(RE::TESObjectCELL*, RE::NiNode*)> attachCell{ REL::RelocationID(18518, 18976) };
		REL::Relocation<std::uint8_t (*)(RE::TESObjectCELL*, bool)> finishAttach{ REL::RelocationID(18594, 19063) };
		loadTempData(a_cell);
		const AttachingScope scope;
		attachCell(a_cell, a_parent);
		finishAttach(a_cell, true);
	}

	void DetachCell(RE::TESObjectCELL* a_cell)
	{
		REL::Relocation<void (*)(RE::TESObjectCELL*, bool)> detachCell{ REL::RelocationID(18655, 19130) };
		detachCell(a_cell, true);
	}

	bool PrepareWorldSpace(RE::TESWorldSpace* a_world)
	{
		if (a_world->portalGraph) {
			return true;
		}
		if (!REL::Module::IsAE()) {
			return false;
		}
		REL::Relocation<void (*)(RE::TESWorldSpace*)> prepare{ REL::RelocationID(0, 20514) };
		prepare(a_world);
		return true;
	}

	bool AttachingRemote()
	{
		return attachingRemote;
	}
}
