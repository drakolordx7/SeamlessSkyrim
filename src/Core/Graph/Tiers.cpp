#include "Core/Graph/Tiers.h"

#include <algorithm>
#include <cfloat>
#include <numeric>

#include "Core/Pictures/Camera.h"
#include "Core/Pictures/Layers.h"

namespace Core::Graph
{
	namespace
	{
		constexpr Tier Next(Tier a_tier) { return static_cast<Tier>(static_cast<int>(a_tier) + 1); }

		// What the scheduler works out for one space behind a door of the player's space.
		struct Candidate
		{
			SpaceIx   space = kNoSpace;
			DoorwayIx via = kNoDoorway;  // the doorway of the best rank
			float     reach = FLT_MAX;      // the nearest door's, gates counted nearer
			float     predicted = FLT_MAX;  // the reach where the player will be in a few seconds
			float     rank = FLT_MAX;
			float     openReach = FLT_MAX;  // the nearest open door's
			Tier      cap = Tier::Cold;
			Tier      current = Tier::Cold;
			Tier      base = Tier::Cold;  // the picture tiers by distance alone
			Tier      want = Tier::Cold;
			Reason    limit = Reason::Approach;
			bool      limited = false;
			float     liveRank = 0.0f;    // the rank as the choice of the live space sees it
			float     loadedRank = 0.0f;  // and as the choice of the loaded ones does
		};

		// Where a step down from a tier goes.
		constexpr Tier StepDown(const Candidate& a_candidate)
		{
			switch (a_candidate.want) {
			case Tier::Live:
				return Tier::LoadedPaused;
			case Tier::LoadedPaused:
				return std::min(Tier::DepthImage, a_candidate.base);
			case Tier::DepthImage:
				return Tier::Image;
			default:
				return Tier::Cold;
			}
		}
	}

	SpaceCost SpaceCost::ForPictures(int a_pictureHeight, int a_depthLayers)
	{
		constexpr float kMegabyte = 1024.0f * 1024.0f;
		constexpr float kMips = 4.0f / 3.0f;
		const auto [imageWidth, imageHeight] = Pictures::Camera::SizeFor(Pictures::Camera::kRoomCamera.extent, a_pictureHeight, Pictures::Camera::kNarrowest);
		const auto [layerWidth, layerHeight] = Pictures::Layers::SizeOf(a_pictureHeight);
		SpaceCost  cost;
		cost.imageMb = static_cast<float>(imageWidth) * static_cast<float>(imageHeight) * Pictures::Camera::kBytesPerTexel * kMips / kMegabyte;
		cost.depthMb = cost.imageMb + static_cast<float>(a_depthLayers) * static_cast<float>(layerWidth) * static_cast<float>(layerHeight) * Pictures::Camera::kBytesPerTexel * kMips / kMegabyte;
		return cost;
	}

	TierScheduler::TierScheduler(const SpaceGraph& a_graph, const SchedulerConfig& a_config) :
		graph(&a_graph),
		config(a_config),
		tiers(a_graph.SpaceCount(), Tier::Cold),
		retryCooldown(a_graph.SpaceCount(), 0.0f),
		heldFor(a_graph.SpaceCount(), 0.0f),
		releasedFor(a_graph.SpaceCount(), FLT_MAX),
		forced(a_graph.DoorwayCount(), false),
		open(a_graph.DoorCount(), false),
		blocked(a_graph.DoorCount(), false),
		candidateOf(a_graph.SpaceCount(), -1)
	{}

	Tier TierScheduler::CapOf(DoorwayIx a_doorway) const
	{
		if (forced[a_doorway]) {
			return Tier::Cold;
		}
		const auto& way = graph->GetDoorway(a_doorway);
		auto        cap = Tier::Live;
		if (way.scripted) {
			cap = std::min(cap, config.scriptedCap);
		}
		if (way.locked) {
			cap = std::min(cap, config.lockedCap);
		}
		return cap;
	}

	const SpaceCost& TierScheduler::CostOf(const Inputs& a_inputs, SpaceIx a_space) const
	{
		return a_space < a_inputs.costs.size() ? a_inputs.costs[a_space] : config.defaultCost;
	}

	void TierScheduler::Failed(SpaceIx a_space, Tier a_reached)
	{
		if (tiers[a_space] >= Tier::LoadedPaused && a_reached < Tier::LoadedPaused) {
			heldFor[a_space] = 0.0f;
		}
		tiers[a_space] = std::min(tiers[a_space], a_reached);
		retryCooldown[a_space] = config.retrySeconds;
	}

	void TierScheduler::Fallback(DoorwayIx a_doorway)
	{
		forced[a_doorway] = true;
	}

	void TierScheduler::Reset()
	{
		std::ranges::fill(tiers, Tier::Cold);
		std::ranges::fill(retryCooldown, 0.0f);
		std::ranges::fill(heldFor, 0.0f);
		std::ranges::fill(releasedFor, FLT_MAX);
		attachTimer = 0.0f;
		memoryMb = 0.0f;
	}

	void TierScheduler::Rebind(const SpaceGraph& a_old, const SpaceGraph& a_new)
	{
		std::vector<Tier>  newTiers(a_new.SpaceCount(), Tier::Cold);
		std::vector<float> newCooldown(a_new.SpaceCount(), 0.0f);
		std::vector<float> newHeld(a_new.SpaceCount(), 0.0f);
		std::vector<float> newReleased(a_new.SpaceCount(), FLT_MAX);
		for (SpaceIx space = 0; space < a_old.SpaceCount(); ++space) {
			if (const auto next = a_new.FindSpace(a_old.GetSpace(space).id); next != kNoSpace) {
				newTiers[next] = tiers[space];
				newCooldown[next] = retryCooldown[space];
				newHeld[next] = heldFor[space];
				newReleased[next] = releasedFor[space];
			}
		}
		std::vector<bool> newForced(a_new.DoorwayCount(), false);
		for (DoorwayIx way = 0; way < a_old.DoorwayCount(); ++way) {
			if (!forced[way]) {
				continue;
			}
			if (const auto door = a_new.FindDoor(a_old.GetDoor(a_old.GetDoorway(way).a).id); door != kNoDoor) {
				newForced[a_new.GetDoor(door).doorway] = true;
			}
		}
		graph = &a_new;
		tiers = std::move(newTiers);
		retryCooldown = std::move(newCooldown);
		heldFor = std::move(newHeld);
		releasedFor = std::move(newReleased);
		forced = std::move(newForced);
		open.assign(a_new.DoorCount(), false);
		blocked.assign(a_new.DoorCount(), false);
		candidateOf.assign(a_new.SpaceCount(), -1);
	}

	std::vector<TierChange> TierScheduler::Tick(const Inputs& a_in)
	{
		std::vector<TierChange> changes;
		attachTimer -= a_in.dt;
		for (auto& cooldown : retryCooldown) {
			cooldown -= a_in.dt;
		}
		for (SpaceIx space = 0; space < tiers.size(); ++space) {
			if (tiers[space] >= Tier::LoadedPaused) {
				heldFor[space] += a_in.dt;
			} else if (releasedFor[space] < FLT_MAX) {
				releasedFor[space] += a_in.dt;
			}
		}
		for (const auto door : a_in.openDoors) {
			open[door] = true;
		}
		for (const auto door : a_in.blockedDoors) {
			blocked[door] = true;
		}

		// Which way the player faces (their movement when no facing is given) and how fast they move.
		auto heading = a_in.heading;
		auto walk = a_in.velocity;
		walk.z = 0.0f;
		if (walk.Length() > Selection::kMaxSpeed) {
			walk = walk / walk.Length() * Selection::kMaxSpeed;
		}
		if (heading.SqrLength() == 0.0f && walk.SqrLength() > 0.0f) {
			heading = walk / walk.Length();
		}
		const auto  player = a_in.playerSpace;
		const float loadReach = Selection::NearRadius(config.liveReach);

		// ---- the spaces behind the doors of the player's space
		std::vector<Candidate> candidates;
		if (player != kNoSpace) {
			for (const auto wayIx : graph->DoorwaysOf(player)) {
				const auto& way = graph->GetDoorway(wayIx);
				const auto  other = way.Other(player);
				if (other == kNoSpace || other == player) {
					continue;
				}
				const auto cap = CapOf(wayIx);
				if (cap == Tier::Cold) {
					continue;
				}
				// A one-way or broken door leads out of its own space only: from the other side there is no door.
				const auto doorIx = way.DoorIn(player);
				if (doorIx == kNoDoor) {
					continue;
				}
				const auto otherEnd = way.DoorIn(other);
				if (blocked[doorIx] || (otherEnd != kNoDoor && blocked[otherEnd])) {
					continue;
				}
				const auto  toDoor = graph->GetDoor(doorIx).pos - a_in.position;
				const float distance = toDoor.Length();
				const bool  gate = way.kind == DoorKind::Gate;
				const float reach = Selection::Reach(distance, gate);
				if (reach > config.imageReach + Selection::kWarmMargin) {
					continue;
				}
				const float closing = distance > 1.0f ? std::max(0.0f, walk.Dot(toDoor) / distance) : 0.0f;
				const float predicted = std::max(0.0f, reach - closing * config.leadSeconds * (gate ? Selection::kReachWeight : 1.0f));
				const float rank = Selection::Rank(predicted, heading, toDoor);
				const bool  isOpen = open[doorIx] || (otherEnd != kNoDoor && open[otherEnd]);

				auto& slot = candidateOf[other];
				if (slot < 0) {
					slot = static_cast<int>(candidates.size());
					candidates.push_back({ .space = other, .current = tiers[other] });
				}
				auto& c = candidates[slot];
				if (rank < c.rank) {
					c.rank = rank;
					c.via = wayIx;
				}
				c.reach = std::min(c.reach, reach);
				c.predicted = std::min(c.predicted, predicted);
				if (isOpen && cap == Tier::Live) {
					c.openReach = std::min(c.openReach, reach);
				}
				c.cap = std::max(c.cap, cap);
			}
		}

		// ---- the tier each would have by distance and heading alone
		std::vector<int> liveEligible;
		std::vector<int> loadedWanted;
		for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
			auto& c = candidates[i];
			auto  t = Tier::Cold;
			if (c.reach <= config.imageReach + (c.current >= Tier::Image ? Selection::kWarmMargin : 0.0f)) {
				t = Tier::Image;
			}
			if (c.reach <= config.depthReach + (c.current >= Tier::DepthImage ? Selection::kWarmMargin : 0.0f)) {
				t = Tier::DepthImage;
			}
			c.base = std::min(t, c.cap);
			if (t != Tier::Cold && c.predicted <= loadReach + (c.current >= Tier::LoadedPaused ? Selection::kReleaseBeyond : 0.0f)) {
				t = Tier::LoadedPaused;
			}
			if (c.openReach <= config.liveReach + (c.current == Tier::Live ? Selection::kHeadingNudge : 0.0f)) {
				t = Tier::Live;
			}
			c.want = std::min(t, c.cap);
			// A space that holds the tier already is kept over a newcomer unless the newcomer is clearly likelier.
			c.liveRank = c.rank - (c.current == Tier::Live ? Selection::kSwitchMargin : 0.0f);
			c.loadedRank = c.rank - (c.current >= Tier::LoadedPaused ? Selection::kSwitchMargin : 0.0f);
			if (c.want == Tier::Live) {
				liveEligible.push_back(i);
			} else if (c.want == Tier::LoadedPaused) {
				loadedWanted.push_back(i);
			}
		}
		const auto byLiveRank = [&](int a_lhs, int a_rhs) { return candidates[a_lhs].liveRank < candidates[a_rhs].liveRank; };
		const auto byLoadedRank = [&](int a_lhs, int a_rhs) { return candidates[a_lhs].loadedRank < candidates[a_rhs].loadedRank; };

		// ---- live: the likeliest open doors, as many as allowed, if there is frame time for them
		std::ranges::stable_sort(liveEligible, byLiveRank);
		std::uint32_t live = 0;
		float         spare = a_in.headroomMs;
		for (const int i : liveEligible) {
			auto&       c = candidates[i];
			const auto& cost = CostOf(a_in, c.space);
			bool        fits = false;
			if (live < config.maxLive) {
				fits = c.current == Tier::Live ? a_in.headroomMs >= 0.0f : spare >= cost.liveMs + config.liveReserveMs;
				if (fits) {
					spare -= c.current == Tier::Live ? 0.0f : cost.liveMs;
					++live;
				} else {
					c.limited = true;
					c.limit = Reason::Headroom;
				}
			}
			if (!fits) {
				c.want = Tier::LoadedPaused;
				loadedWanted.push_back(i);
			}
		}

		// ---- loaded and paused: the likeliest few
		std::ranges::stable_sort(loadedWanted, byLoadedRank);
		for (std::size_t n = config.maxLoaded; n < loadedWanted.size(); ++n) {
			auto& c = candidates[loadedWanted[n]];
			c.want = c.base;
		}

		// ---- memory: while over budget the least likely gives way, a tier at a time, until it is cold
		float total = 0.0f;
		for (const auto& c : candidates) {
			total += CostOf(a_in, c.space).Mb(c.want);
		}
		while (total > a_in.memoryBudgetMb) {
			Candidate* worst = nullptr;
			for (auto& c : candidates) {
				if (c.want == Tier::Cold) {
					continue;
				}
				if (!worst || c.rank > worst->rank) {
					worst = &c;
				}
			}
			if (!worst) {
				break;
			}
			const auto& cost = CostOf(a_in, worst->space);
			const auto  lower = StepDown(*worst);
			total -= cost.Mb(worst->want) - cost.Mb(lower);
			worst->want = lower;
			worst->limited = true;
			worst->limit = Reason::Budget;
		}

		// ---- carry it out: the player's space, then what lets go, then what is promoted one tier at a time
		const auto change = [&](SpaceIx a_space, Tier a_to, Reason a_reason, DoorwayIx a_via, float a_rank) {
			changes.push_back({ a_space, tiers[a_space], a_to, a_reason, a_via, a_rank });
			if (tiers[a_space] >= Tier::LoadedPaused && a_to < Tier::LoadedPaused) {
				heldFor[a_space] = 0.0f;
				releasedFor[a_space] = 0.0f;
			}
			tiers[a_space] = a_to;
		};
		if (player != kNoSpace && tiers[player] != Tier::Live) {
			change(player, Tier::Live, Reason::PlayerEntered, kNoDoorway, 0.0f);
		}
		for (SpaceIx space = 0; space < tiers.size(); ++space) {
			if (space == player || tiers[space] == Tier::Cold) {
				continue;
			}
			const int slot = candidateOf[space];
			if (slot < 0) {
				change(space, Tier::Cold, Reason::LeftReach, kNoDoorway, 0.0f);
				continue;
			}
			const auto& c = candidates[slot];
			auto        want = c.want;
			// Kept loaded for a while whatever the ranking says (the doorway still being a way through).
			if (tiers[space] >= Tier::LoadedPaused && want < Tier::LoadedPaused && heldFor[space] < config.minHoldSeconds && c.cap >= Tier::LoadedPaused) {
				want = Tier::LoadedPaused;
			}
			if (want < tiers[space]) {
				const auto reason = c.limited ? c.limit : tiers[space] == Tier::Live && c.openReach == FLT_MAX ? Reason::DoorClosed : Reason::Retreat;
				change(space, want, reason, c.via, c.rank);
			}
		}

		float used = 0.0f;
		for (SpaceIx space = 0; space < tiers.size(); ++space) {
			if (space != player) {
				used += CostOf(a_in, space).Mb(tiers[space]);
			}
		}
		std::vector<int> order(candidates.size());
		std::iota(order.begin(), order.end(), 0);
		std::ranges::stable_sort(order, [&](int a_lhs, int a_rhs) { return candidates[a_lhs].rank < candidates[a_rhs].rank; });
		for (const int i : order) {
			const auto& c = candidates[i];
			const auto  at = tiers[c.space];
			if (c.want <= at || retryCooldown[c.space] > 0.0f) {
				continue;
			}
			const auto  next = Next(at);
			if (next == Tier::LoadedPaused && releasedFor[c.space] < config.reattachSeconds) {
				continue;
			}
			const auto& cost = CostOf(a_in, c.space);
			const float delta = cost.Mb(next) - cost.Mb(at);
			if (used + delta > a_in.memoryBudgetMb) {
				continue;
			}
			if (next == Tier::LoadedPaused) {
				// One attach at a time, and only with frame time to spare for it.
				if (attachTimer > 0.0f || a_in.headroomMs < cost.attachMs) {
					continue;
				}
				attachTimer = config.attachSeconds;
			}
			used += delta;
			change(c.space, next, Reason::Approach, c.via, c.rank);
		}
		memoryMb = used;

		for (const auto& c : candidates) {
			candidateOf[c.space] = -1;
		}
		for (const auto door : a_in.openDoors) {
			open[door] = false;
		}
		for (const auto door : a_in.blockedDoors) {
			blocked[door] = false;
		}
		return changes;
	}
}
