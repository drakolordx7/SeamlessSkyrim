#include "Skyrim/Spaces/RemoteCell.h"

#include "Skyrim/Diag/Probe.h"

namespace Portal
{
	json RemoteCell::RenderPasses(bool a_drop)
	{
		static const auto [begin, end] = [] {
			const auto& module = REL::Module::get();
			return std::pair{ module.base(), module.base() + module.segment(REL::Segment::rdata).offset() + module.segment(REL::Segment::rdata).size() };
		}();
		int shapes = 0, lists = 0, passes = 0, stale = 0, foreign = 0, dropped = 0;
		int         ownFade = 0, otherFade = 0;
		std::string sample;
		const auto check = [&](RE::BSShaderProperty::RenderPassArray& a_list, RE::BSGeometry* a_shape) {
			if (!a_list.head) {
				return;
			}
			++lists;
			bool bad = false;
			int  guard = 0;
			for (auto* pass = a_list.head; pass && guard < 64; pass = pass->next, ++guard) {
				++passes;
				const auto vtable = pass->shader ? *reinterpret_cast<const std::uintptr_t*>(pass->shader) : 0;
				if (vtable < begin || vtable >= end) {
					++stale;
					bad = true;
					break;
				}
				if (pass->geometry != a_shape) {
					++foreign;
					bad = true;
					break;
				}
			}
			if (bad && a_drop) {
				a_list.head = nullptr;
				++dropped;
			}
		};
		const auto visit = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_shape) {
				++shapes;
				if (auto* shader = a_shape->GetGeometryRuntimeData().shaderProperty.get()) {
					RE::BSFadeNode* owner = nullptr;
					for (auto* up = a_shape->parent; up && !owner; up = up->parent) {
						owner = up->AsFadeNode();
					}
					if (shader->fadeNode != owner) {
						++otherFade;
						if (sample.empty()) {
							sample = std::format("'{}' fadeNode {:X} owner {:X} '{}'", a_shape->name.c_str(), reinterpret_cast<std::uintptr_t>(shader->fadeNode),
								reinterpret_cast<std::uintptr_t>(owner), owner ? owner->name.c_str() : "");
						}
					} else {
						++ownFade;
					}
					check(shader->renderPassList, a_shape);
					check(shader->debugRenderPassList, a_shape);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		nlohmann::json tree = nlohmann::json::array();
		for (auto* cell : cells) {
			if (auto* cell3D = Cell3D(cell)) {
				visit(cell3D);
				std::string line = std::format("{:08X} flags {:X} parent '{}'{}:", cell->GetFormID(), cell3D->GetFlags().underlying(), cell3D->parent ? cell3D->parent->name.c_str() : "",
					LightScene::IsOurs(cell3D->parent) ? " (portal scene)" : "");
				if (auto* node = cell3D->AsNode()) {
					for (auto& child : node->GetChildren()) {
						auto* childNode = child ? child->AsNode() : nullptr;
						int below = 0;
						if (child) {
							RE::BSVisit::TraverseScenegraphGeometries(child.get(), [&](RE::BSGeometry*) {
								++below;
								return RE::BSVisit::BSVisitControl::kContinue;
							});
						}
						line += child ? std::format(" [{} {:X} n={} shapes={}]", child->name.c_str(), child->GetFlags().underlying(), childNode ? childNode->GetChildren().size() : 0, below) : " [null]";
					}
				}
				tree.push_back(line);
			}
		}
		for (auto& top : borrowedPersistent) {
			visit(top.get());
		}
		return { { "shapes", shapes }, { "lists", lists }, { "passes", passes }, { "stale", stale }, { "foreign", foreign }, { "dropped", dropped }, { "cells", tree },
			{ "ownFade", ownFade }, { "otherFade", otherFade }, { "sample", sample } };
	}

	json RemoteCell::Describe() const
	{
		auto* cell = Cell();
		if (!cell) {
			return nullptr;
		}
		json actors = json::array();
		for (auto* actor : ActorsIn(cell)) {
			const auto pos = actor->GetPosition();
			actors.push_back({
				{ "name", actor->GetDisplayFullName() },
				{ "has3D", actor->Get3D() != nullptr },
				{ "pos", { pos.x, pos.y, pos.z } },
			});
		}
		return {
			{ "cell", Probe::DescribeCell(cell) },
			{ "neighbours", cells.size() - 1 },
			{ "healthy", Healthy() },
			{ "fullyLoaded", FullyLoaded() },
			{ "loadState", [cell] {
				 auto* loaded = cell->GetRuntimeData().loadedData;
				 return loaded ? json{ { "refsFullyLoaded", loaded->refsFullyLoaded }, { "queued", loaded->queuedRefCount }, { "criticalQueued", loaded->criticalQueuedRefCount }, { "distantQueued", loaded->queuedDistantRefCount } } : json(nullptr);
			 }() },
			{ "adoptedLights", adoptedLights },
			{ "fade", [this] {
				 // Diagnostics: BSFadeNodes by current fade (0 = invisible, 1 = fully shown).
				 std::uint32_t hidden = 0, partial = 0, shown = 0;
				 std::function<void(RE::NiAVObject*)> visit = [&](RE::NiAVObject* a_object) {
					 if (!a_object) {
						 return;
					 }
					 if (auto* fade = a_object->AsFadeNode()) {
						 const float value = fade->GetRuntimeData().currentFade;
						 (value <= 0.0f ? hidden : value >= 1.0f ? shown : partial)++;
					 }
					 if (auto* node = a_object->AsNode()) {
						 for (auto& child : node->GetChildren()) {
							 visit(child.get());
						 }
					 }
				 };
				 visit(Cell3D());
				 return json{ { "hidden", hidden }, { "partial", partial }, { "shown", shown } };
			 }() },
			{ "steppedFrames", steppedFrames.load() },
			{ "actors", actors },
		};
	}
}
