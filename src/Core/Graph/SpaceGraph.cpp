#include "Core/Graph/SpaceGraph.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace Core::Graph
{
	SpaceGraph SpaceGraph::Build(std::span<const SpaceRecord> a_spaces, std::span<const DoorRecord> a_doors, const BuildOptions& a_options)
	{
		SpaceGraph graph;
		auto&      spaces = graph.spaces;
		auto&      doors = graph.doors;
		auto&      doorways = graph.doorways;

		const auto intern = [&](const std::string& a_id, SpaceKind a_kindIfNew) {
			if (const auto ix = graph.FindSpace(a_id); ix != kNoSpace) {
				return ix;
			}
			++graph.stats.unlistedSpaces;
			const auto ix = static_cast<SpaceIx>(spaces.size());
			graph.spaceIndex.emplace(a_id, ix);
			spaces.push_back({ .id = a_id, .kind = a_kindIfNew });
			return ix;
		};

		// ---- spaces, then their parents (which may be unlisted)
		for (const auto& record : a_spaces) {
			if (graph.FindSpace(record.id) != kNoSpace) {
				++graph.stats.duplicateSpaces;
				continue;
			}
			graph.spaceIndex.emplace(record.id, static_cast<SpaceIx>(spaces.size()));
			spaces.push_back({ .id = record.id, .kind = record.kind });
		}
		for (const auto& record : a_spaces) {
			if (!record.parent.empty()) {
				const auto parent = intern(record.parent, SpaceKind::Worldspace);
				const auto child = graph.FindSpace(record.id);
				spaces[child].parent = parent;
			}
		}

		// ---- doors (a duplicate id keeps the first record)
		std::vector<const DoorRecord*> kept;
		kept.reserve(a_doors.size());
		for (const auto& record : a_doors) {
			if (graph.FindDoor(record.id) != kNoDoor) {
				++graph.stats.duplicateDoors;
				continue;
			}
			const auto space = intern(record.space, SpaceKind::Interior);
			const auto ix = static_cast<DoorIx>(doors.size());
			graph.doorIndex.emplace(record.id, ix);
			doors.push_back({ .id = record.id,
				.space = space,
				.pos = record.pos,
				.rot = record.rot,
				.halfWidth = record.halfWidth,
				.halfHeight = record.halfHeight,
				.landPos = record.landPos,
				.landRot = record.landRot,
				.kind = record.kind,
				.locked = record.locked,
				.scripted = record.scripted,
				.baseScripted = record.baseScripted,
				.minimalUse = record.minimalUse,
				.automatic = record.automatic,
				.hidden = record.hidden,
				.openByDefault = record.openByDefault });
			spaces[space].doors.push_back(ix);
			kept.push_back(&record);
		}

		// ---- doorways: two doors that lead to each other make one; any other door stands alone
		std::vector<DoorIx> destOf(doors.size(), kNoDoor);
		for (DoorIx ix = 0; ix < doors.size(); ++ix) {
			destOf[ix] = kept[ix]->destDoor.empty() ? kNoDoor : graph.FindDoor(kept[ix]->destDoor);
		}
		std::vector<bool> taken(doors.size(), false);
		for (DoorIx ix = 0; ix < doors.size(); ++ix) {
			if (taken[ix]) {
				continue;
			}
			Doorway    way;
			const auto other = destOf[ix];
			way.a = ix;
			way.spaceA = doors[ix].space;
			if (other != kNoDoor && other != ix && destOf[other] == ix && !taken[other]) {
				way.b = other;
				way.spaceB = doors[other].space;
				way.link = Link::Paired;
				taken[other] = true;
			} else if (other != kNoDoor) {
				way.spaceB = doors[other].space;
				way.link = Link::OneWay;
			} else {
				way.spaceB = kept[ix]->destSpace.empty() ? kNoSpace : intern(kept[ix]->destSpace, SpaceKind::Interior);
				way.link = Link::Broken;
			}
			taken[ix] = true;

			// Between two worldspaces it is a gate whatever the record says; otherwise the odd kind of the two ends.
			way.kind = doors[ix].kind;
			if (way.b != kNoDoor && way.kind == DoorKind::Hinged) {
				way.kind = doors[way.b].kind;
			}
			if (way.spaceB != kNoSpace && spaces[way.spaceA].kind == SpaceKind::Worldspace && spaces[way.spaceB].kind == SpaceKind::Worldspace) {
				way.kind = DoorKind::Gate;
			}
			for (const auto end : { way.a, way.b }) {
				if (end != kNoDoor) {
					way.locked |= doors[end].locked;
					way.scripted |= doors[end].scripted || doors[end].baseScripted;
				}
			}

			const auto wayIx = static_cast<DoorwayIx>(doorways.size());
			doors[way.a].doorway = wayIx;
			doors[way.a].kind = way.kind;
			if (way.b != kNoDoor) {
				doors[way.b].doorway = wayIx;
				doors[way.b].kind = way.kind;
			}
			spaces[way.spaceA].doorways.push_back(wayIx);
			if (way.spaceB != kNoSpace && way.spaceB != way.spaceA) {
				spaces[way.spaceB].doorways.push_back(wayIx);
			}
			doorways.push_back(way);
		}

		// ---- child worldspaces: do the gate pairs line up with the parent's coordinates
		for (auto& space : spaces) {
			if (space.kind != SpaceKind::Worldspace || space.parent == kNoSpace) {
				continue;
			}
			int pairs = 0;
			int shared = 0;
			for (const auto wayIx : space.doorways) {
				const auto& way = doorways[wayIx];
				if (way.kind != DoorKind::Gate || way.link != Link::Paired) {
					continue;
				}
				auto step = doors[way.a].pos - doors[way.b].pos;
				step.z = 0.0f;
				++pairs;
				shared += step.Length() <= a_options.sharedThreshold ? 1 : 0;
			}
			space.coordinates = pairs == 0 ? Coordinates::NoGates : shared == pairs ? Coordinates::Shared : shared == 0 ? Coordinates::Separate : Coordinates::Partial;
		}
		return graph;
	}

	std::vector<SpaceIx> SpaceGraph::Neighbours(SpaceIx a_space) const
	{
		std::vector<SpaceIx> out;
		for (const auto wayIx : spaces[a_space].doorways) {
			const auto other = doorways[wayIx].Other(a_space);
			if (other != kNoSpace && other != a_space) {
				out.push_back(other);
			}
		}
		std::ranges::sort(out);
		out.erase(std::ranges::unique(out).begin(), out.end());
		return out;
	}

	std::optional<std::vector<DoorwayIx>> SpaceGraph::Path(SpaceIx a_from, SpaceIx a_to, bool a_skipLocked) const
	{
		if (a_from == a_to) {
			return std::vector<DoorwayIx>{};
		}
		// For each space reached: the doorway it was reached through.
		std::vector<DoorwayIx> via(spaces.size(), kNoDoorway);
		std::vector<bool>      seen(spaces.size(), false);
		std::deque<SpaceIx>    queue{ a_from };
		seen[a_from] = true;
		while (!queue.empty()) {
			const auto at = queue.front();
			queue.pop_front();
			for (const auto wayIx : spaces[at].doorways) {
				const auto& way = doorways[wayIx];
				const auto  next = way.Other(at);
				if (next == kNoSpace || seen[next] || (a_skipLocked && way.locked)) {
					continue;
				}
				seen[next] = true;
				via[next] = wayIx;
				if (next == a_to) {
					std::vector<DoorwayIx> path;
					for (auto back = a_to; back != a_from; back = doorways[via[back]].Other(back)) {
						path.push_back(via[back]);
					}
					std::ranges::reverse(path);
					return path;
				}
				queue.push_back(next);
			}
		}
		return std::nullopt;
	}

	std::vector<DoorHit> SpaceGraph::DoorsWithin(SpaceIx a_space, const Vec3& a_point, float a_radius) const
	{
		std::vector<DoorHit> out;
		const float          sqrRadius = a_radius * a_radius;
		for (const auto doorIx : spaces[a_space].doors) {
			const float sqr = (doors[doorIx].pos - a_point).SqrLength();
			if (sqr <= sqrRadius) {
				out.push_back({ doorIx, doors[doorIx].doorway, std::sqrt(sqr) });
			}
		}
		std::ranges::sort(out, {}, &DoorHit::distance);
		return out;
	}
}
