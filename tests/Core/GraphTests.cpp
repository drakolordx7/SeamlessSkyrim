// Tests of the space graph, the tier scheduler and the crossing sequence: plain data in, no game.
// The census test reads the door census JSON (tools/census) if it is there and skips otherwise.
#define _CRT_SECURE_NO_WARNINGS  // getenv
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Crossing/Sequence.h"
#include "Core/Graph/SpaceGraph.h"
#include "Core/Graph/Tiers.h"
#include "GraphTests.h"

namespace
{
	using namespace Core;
	using namespace Core::Graph;

	int failures = 0;

#define CHECK(a_condition)                                                              \
	do {                                                                                \
		if (!(a_condition)) {                                                           \
			std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #a_condition);       \
			++failures;                                                                 \
		}                                                                               \
	} while (false)

	void Run(const char* a_name, const std::function<void()>& a_test)
	{
		const int before = failures;
		a_test();
		std::printf("%s %s\n", failures == before ? "ok    " : "FAILED", a_name);
	}

	// ---- fixtures

	SpaceRecord Space(std::string a_id, SpaceKind a_kind, std::string a_parent = {})
	{
		return { std::move(a_id), a_kind, std::move(a_parent) };
	}

	// Both ends of a doorway.
	void Pair(std::vector<DoorRecord>& a_doors, const std::string& a_nameA, const std::string& a_spaceA, Vec3 a_posA, const std::string& a_nameB, const std::string& a_spaceB, Vec3 a_posB)
	{
		a_doors.push_back({ .id = a_nameA, .space = a_spaceA, .pos = a_posA, .destDoor = a_nameB });
		a_doors.push_back({ .id = a_nameB, .space = a_spaceB, .pos = a_posB, .destDoor = a_nameA });
	}

	struct Fixture
	{
		std::vector<SpaceRecord> spaces;
		std::vector<DoorRecord>  doors;
	};

	// A village: Tamriel with a house, an inn (and a room inside it), a locked cave and a walled
	// city over its own footprint; an island nobody can reach; a one-way and a broken door.
	Fixture Village()
	{
		Fixture f;
		f.spaces = { Space("tamriel", SpaceKind::Worldspace), Space("house", SpaceKind::Interior), Space("inn", SpaceKind::Interior), Space("room", SpaceKind::Interior),
			Space("cave", SpaceKind::Interior), Space("city", SpaceKind::Worldspace, "tamriel"), Space("island", SpaceKind::Interior), Space("far", SpaceKind::Worldspace, "tamriel") };
		Pair(f.doors, "houseIn", "tamriel", { 1000, 0, 0 }, "houseOut", "house", { 0, 0, 0 });
		Pair(f.doors, "innIn", "tamriel", { 0, 1000, 0 }, "innOut", "inn", { 0, 0, 0 });
		Pair(f.doors, "roomIn", "inn", { 300, 0, 0 }, "roomOut", "room", { 0, 0, 0 });
		Pair(f.doors, "caveIn", "tamriel", { 5000, 0, 0 }, "caveOut", "cave", { 0, 0, 0 });
		f.doors.back().locked = true;
		// the city's gate pair stands at nearly the same place in both worldspaces
		Pair(f.doors, "gateOut", "tamriel", { -1000, 0, 0 }, "gateIn", "city", { -900, 0, 0 });
		// a gate to a worldspace with its own coordinates
		Pair(f.doors, "farOut", "tamriel", { -2000, 0, 0 }, "farIn", "far", { 90000, 0, 0 });
		// one-way: leads to the house door, which leads elsewhere; broken: the destination is not in the list
		f.doors.push_back({ .id = "oneWay", .space = "inn", .pos = { 50, 0, 0 }, .destDoor = "houseIn" });
		f.doors.push_back({ .id = "broken", .space = "inn", .pos = { 60, 0, 0 }, .destDoor = "nowhere" });
		f.doors.push_back({ .id = "scripted", .space = "house", .pos = { 10, 0, 0 }, .destDoor = "nowhere", .destSpace = "tamriel", .scripted = true });
		return f;
	}

	// A street of three houses beside each other, all doors closed.
	Fixture Street()
	{
		Fixture f;
		f.spaces = { Space("street", SpaceKind::Worldspace), Space("a", SpaceKind::Interior), Space("b", SpaceKind::Interior), Space("c", SpaceKind::Interior) };
		Pair(f.doors, "doorA", "street", { 1000, 0, 0 }, "backA", "a", { 0, 0, 0 });
		Pair(f.doors, "doorB", "street", { 1000, 300, 0 }, "backB", "b", { 0, 0, 0 });
		Pair(f.doors, "doorC", "street", { 1000, -300, 0 }, "backC", "c", { 0, 0, 0 });
		return f;
	}

	// Every consecutive step of a path joins the two spaces.
	bool ValidPath(const SpaceGraph& a_graph, SpaceIx a_from, SpaceIx a_to, const std::vector<DoorwayIx>& a_path)
	{
		auto at = a_from;
		for (const auto way : a_path) {
			const auto next = a_graph.GetDoorway(way).Other(at);
			if (a_graph.GetDoorway(way).spaceA != at && a_graph.GetDoorway(way).spaceB != at) {
				return false;
			}
			at = next;
		}
		return at == a_to;
	}

	// ---- the graph

	void GraphFixture()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		CHECK(g.SpaceCount() == 8 && g.DoorCount() == f.doors.size() && g.Stats().unlistedSpaces == 0);
		const auto tamriel = g.FindSpace("tamriel");
		const auto house = g.FindSpace("house");
		const auto inn = g.FindSpace("inn");
		const auto room = g.FindSpace("room");
		const auto cave = g.FindSpace("cave");
		const auto city = g.FindSpace("city");
		const auto island = g.FindSpace("island");
		CHECK(g.FindSpace("nope") == kNoSpace && g.FindDoor("nope") == kNoDoor);

		// six pairs, plus the one-way, the broken and the scripted door (broken, its space known)
		CHECK(g.DoorwayCount() == 9);
		CHECK(g.GetDoor(g.FindDoor("houseIn")).doorway == g.GetDoor(g.FindDoor("houseOut")).doorway);
		const auto& oneWay = g.GetDoorway(g.GetDoor(g.FindDoor("oneWay")).doorway);
		CHECK(oneWay.link == Link::OneWay && oneWay.spaceB == tamriel && oneWay.b == kNoDoor);
		const auto& broken = g.GetDoorway(g.GetDoor(g.FindDoor("broken")).doorway);
		CHECK(broken.link == Link::Broken && broken.spaceB == kNoSpace);
		const auto& scripted = g.GetDoorway(g.GetDoor(g.FindDoor("scripted")).doorway);
		CHECK(scripted.link == Link::Broken && scripted.spaceB == tamriel && scripted.scripted);

		// doors and doorways of a space
		CHECK(g.DoorsOf(tamriel).size() == 5 && g.DoorsOf(island).empty());
		CHECK(g.DoorwaysOf(tamriel).size() == 7);  // five paired, the one-way and the scripted door's (both lead into it)

		// neighbours: each once, sorted, never itself
		const auto near = g.Neighbours(tamriel);
		CHECK((near == std::vector<SpaceIx>{ house, inn, cave, city, g.FindSpace("far") }));
		CHECK(g.Neighbours(island).empty());
		CHECK(g.Neighbours(room) == std::vector<SpaceIx>{ inn });

		// kinds: a worldspace to a worldspace is a gate, whatever the record says
		CHECK(g.GetDoorway(g.GetDoor(g.FindDoor("gateOut")).doorway).kind == DoorKind::Gate);
		CHECK(g.GetDoor(g.FindDoor("gateIn")).kind == DoorKind::Gate);
		CHECK(g.GetDoorway(g.GetDoor(g.FindDoor("houseIn")).doorway).kind == DoorKind::Hinged);
		CHECK(g.GetDoorway(g.GetDoor(g.FindDoor("caveIn")).doorway).locked);

		// coordinate sharing: the city lines up with Tamriel, the far worldspace does not, a space without gates is NoGates
		CHECK(g.GetSpace(city).coordinates == Coordinates::Shared && g.GetSpace(city).CoordinateShared() && g.GetSpace(city).parent == tamriel);
		CHECK(g.GetSpace(g.FindSpace("far")).coordinates == Coordinates::Separate);
		CHECK(g.GetSpace(tamriel).coordinates == Coordinates::NoGates && g.GetSpace(house).parent == kNoSpace);

		// paths: fewest doorways, empty for the same space, none to an unreachable one or past a locked door
		const auto toRoom = g.Path(tamriel, room);
		CHECK(toRoom && toRoom->size() == 2 && ValidPath(g, tamriel, room, *toRoom));
		CHECK(g.GetDoorway((*toRoom)[0]).Other(tamriel) == inn);
		const auto back = g.Path(room, house);
		CHECK(back && back->size() == 3 && ValidPath(g, room, house, *back));
		CHECK(g.Path(house, house) && g.Path(house, house)->empty());
		CHECK(!g.Path(tamriel, island));
		CHECK(g.Path(house, cave) && !g.Path(house, cave, true));

		// doorways within a radius of a point, nearest first, only those standing in the space
		const auto hits = g.DoorsWithin(tamriel, { 900, 0, 0 }, 2000);  // house door 100, inn door 1345, gate 1900; the rest further
		CHECK(hits.size() == 3 && g.GetDoor(hits[0].door).id == "houseIn" && g.GetDoor(hits[1].door).id == "innIn" && g.GetDoor(hits[2].door).id == "gateOut");
		const auto close = g.DoorsWithin(tamriel, { 900, 0, 0 }, 150);
		CHECK(close.size() == 1 && g.GetDoor(close[0].door).id == "houseIn" && close[0].distance > 99.9f && close[0].distance < 100.1f && close[0].doorway == g.GetDoor(close[0].door).doorway);
		CHECK(g.DoorsWithin(house, { 900, 0, 0 }, 150).empty());
	}

	// ---- the census

	std::string CensusPath()
	{
		if (const char* env = std::getenv("SEAMLESS_CENSUS_JSON")) {
			return env;
		}
#ifdef SEAMLESS_CENSUS_JSON
		return SEAMLESS_CENSUS_JSON;
#else
		return {};
#endif
	}

	Vec3 ToVec(const nlohmann::json& a_array)
	{
		return { a_array[0].get<float>(), a_array[1].get<float>(), a_array[2].get<float>() };
	}

	// The census schema (door-census/1) to the graph's records: what a runtime scan would produce too.
	Fixture FromCensus(const nlohmann::json& a_census)
	{
		Fixture f;
		for (const auto& s : a_census.at("spaces")) {
			f.spaces.push_back({ s.at("id"), s.at("kind") == "interior" ? SpaceKind::Interior : SpaceKind::Worldspace, s.value("parentId", "") });
		}
		for (const auto& d : a_census.at("doors")) {
			DoorRecord r;
			r.id = d.at("id");
			r.space = d.at("at").at("spaceId");
			r.pos = ToVec(d.at("pos"));
			r.rot = ToVec(d.at("rot"));
			const auto& dest = d.at("dest");
			r.destDoor = dest.value("doorId", "");
			if (dest.contains("at")) {
				r.destSpace = dest.at("at").at("spaceId");
			}
			r.landPos = ToVec(dest.at("landPos"));
			r.landRot = ToVec(dest.at("landRot"));
			const auto& base = d.at("base");
			std::string model = base.value("model", "");
			std::ranges::transform(model, model.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			if (model.contains("trapdoor") || model.contains("hatch")) {
				r.kind = DoorKind::Hatch;
			}
			r.locked = d.contains("lock");
			r.scripted = d.value("hasScripts", false);
			r.baseScripted = d.value("baseHasScripts", false);
			r.minimalUse = d.value("minimalUse", false);
			r.automatic = d.value("automatic", false);
			r.hidden = d.value("initiallyDisabled", false);
			r.openByDefault = d.value("openByDefault", false);
			f.doors.push_back(std::move(r));
		}
		return f;
	}

	void CensusGraph()
	{
		const auto path = CensusPath();
		std::ifstream file(path);
		if (path.empty() || !file) {
			std::printf("  (census not found, set SEAMLESS_CENSUS_JSON: skipped)\n");
			return;
		}
		const auto census = nlohmann::json::parse(file);
		const auto f = FromCensus(census);
		const auto g = SpaceGraph::Build(f.spaces, f.doors);

		// the numbers of the census summary
		std::size_t interiors = 0, worldspaces = 0, children = 0;
		for (SpaceIx i = 0; i < g.SpaceCount(); ++i) {
			const auto& s = g.GetSpace(i);
			interiors += s.kind == SpaceKind::Interior;
			worldspaces += s.kind == SpaceKind::Worldspace;
			children += s.parent != kNoSpace;
		}
		CHECK(g.SpaceCount() == 990 && interiors == 938 && worldspaces == 52 && children == 45 && g.Stats().unlistedSpaces == 0);
		CHECK(g.DoorCount() == 2646 && g.Stats().duplicateDoors == 0);

		std::size_t locked = 0, scripted = 0, baseScripted = 0, minimal = 0, automatic = 0, gateDoors = 0;
		for (DoorIx i = 0; i < g.DoorCount(); ++i) {
			const auto& d = g.GetDoor(i);
			locked += d.locked;
			scripted += d.scripted;
			baseScripted += d.baseScripted;
			minimal += d.minimalUse;
			automatic += d.automatic;
			gateDoors += d.kind == DoorKind::Gate;
			CHECK(d.doorway != kNoDoorway);
		}
		CHECK(locked == 340 && scripted == 94 && baseScripted == 38 && minimal == 184 && automatic == 366 && gateDoors == 150);

		// links, by kind of the two spaces: 313 + 935 + 75 + 1 = 1324 (a pair once, a one-way or broken door once)
		std::map<std::string, int> byKind;
		std::map<Link, int>        byLink;
		std::set<std::pair<SpaceIx, SpaceIx>> edges;
		for (DoorwayIx i = 0; i < g.DoorwayCount(); ++i) {
			const auto& way = g.GetDoorway(i);
			++byLink[way.link];
			if (way.spaceB == kNoSpace) {
				++byKind["unknown"];
				continue;
			}
			const bool a = g.GetSpace(way.spaceA).kind == SpaceKind::Interior;
			const bool b = g.GetSpace(way.spaceB).kind == SpaceKind::Interior;
			++byKind[a && b ? "interior-interior" : a || b ? "interior-exterior" : "exterior-exterior"];
			edges.emplace(std::min(way.spaceA, way.spaceB), std::max(way.spaceA, way.spaceB));
		}
		CHECK(g.DoorwayCount() == 1324);
		CHECK(byKind["interior-interior"] == 313 && byKind["interior-exterior"] == 935 && byKind["exterior-exterior"] == 75 && byKind["unknown"] == 1);
		CHECK(byLink[Link::Paired] == 1322 && byLink[Link::OneWay] == 1 && byLink[Link::Broken] == 1);

		// the same space pairs as the census graph's edges
		const auto& edgeList = census.at("graph").at("edges");
		CHECK(edges.size() == edgeList.size());
		std::size_t missing = 0;
		for (const auto& e : edgeList) {
			const auto a = g.FindSpace(e.at("a").get<std::string>());
			const auto b = g.FindSpace(e.at("b").get<std::string>());
			missing += !edges.contains({ std::min(a, b), std::max(a, b) });
		}
		CHECK(missing == 0);

		// child worldspaces: the census's own verdict on each one's coordinates
		std::map<Coordinates, int> modes;
		std::size_t                wrong = 0;
		for (const auto& cw : census.at("graph").at("childWorldspaces")) {
			const auto  mode = g.GetSpace(g.FindSpace(cw.at("id").get<std::string>())).coordinates;
			const auto  want = cw.at("coordinates").get<std::string>();
			const auto  expected = want == "shared" ? Coordinates::Shared : want == "partial" ? Coordinates::Partial : want == "separate" ? Coordinates::Separate : Coordinates::NoGates;
			wrong += mode != expected;
			++modes[mode];
		}
		CHECK(wrong == 0 && modes[Coordinates::Shared] == 6 && modes[Coordinates::Partial] == 1 && modes[Coordinates::Separate] == 27 && modes[Coordinates::NoGates] == 11);

		// paths: every interior with a door reaches the main worldspace, by a chain that joins up
		const auto tamriel = g.FindSpace("worldspace:Skyrim.esm:00003C");
		CHECK(tamriel != kNoSpace);
		std::size_t reached = 0, tried = 0, short1 = 0;
		for (SpaceIx i = 0; i < g.SpaceCount() && tried < 300; ++i) {
			if (g.GetSpace(i).kind != SpaceKind::Interior || g.GetSpace(i).doorways.empty()) {
				continue;
			}
			++tried;
			if (const auto p = g.Path(i, tamriel); p && ValidPath(g, i, tamriel, *p)) {
				++reached;
				short1 += p->size() == 1;
			}
		}
		CHECK(tried > 100 && reached > tried * 3 / 4 && short1 > 0);
		// an interior with a door straight to the worldspace: one doorway, and the door in the worldspace is within radius of itself
		const auto& towns = g.GetSpace(tamriel).doors;
		CHECK(!towns.empty());
		const auto  door = towns.front();
		const auto  hits = g.DoorsWithin(tamriel, g.GetDoor(door).pos, 1.0f);
		CHECK(!hits.empty() && hits.front().distance < 1.0f);
	}

	// ---- the scheduler

	struct Walk
	{
		const SpaceGraph* graph;
		TierScheduler     scheduler;
		Inputs            in;
		std::vector<DoorIx> open;

		explicit Walk(const SpaceGraph& a_graph, const SchedulerConfig& a_config = {}) :
			graph(&a_graph),
			scheduler(a_graph, a_config)
		{
			in.dt = 1.0f / 60.0f;
			in.headroomMs = 10.0f;
			in.memoryBudgetMb = 1000.0f;
		}

		std::vector<TierChange> Step()
		{
			in.openDoors = open;
			return scheduler.Tick(in);
		}
		// Standing still for a_seconds.
		std::vector<TierChange> Stand(float a_seconds)
		{
			in.velocity = {};
			std::vector<TierChange> all;
			for (int i = 0; i < static_cast<int>(a_seconds * 60.0f); ++i) {
				auto c = Step();
				all.insert(all.end(), c.begin(), c.end());
			}
			return all;
		}
		void Open(std::string_view a_door) { open.push_back(graph->FindDoor(a_door)); }
	};

	std::vector<Tier> TiersOf(const std::vector<TierChange>& a_changes, SpaceIx a_space)
	{
		std::vector<Tier> out;
		for (const auto& c : a_changes) {
			if (c.space == a_space) {
				out.push_back(c.to);
			}
		}
		return out;
	}

	void SchedulerApproach()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto tamriel = g.FindSpace("tamriel");
		const auto house = g.FindSpace("house");
		Walk       walk(g);
		walk.in.playerSpace = tamriel;
		walk.in.heading = { 1, 0, 0 };

		// Walking up to the house door (at x = 1000) at 300 units a second, the door opens at 600 units.
		std::vector<TierChange> houseChanges;
		float                   liveAt = 0.0f;
		for (float x = -3500.0f; x < 950.0f; x += 5.0f) {
			walk.in.position = { x, 0, 0 };
			walk.in.velocity = { 300, 0, 0 };
			if (x >= 400.0f && walk.open.empty()) {
				walk.Open("houseIn");
			}
			for (const auto& c : walk.Step()) {
				if (c.space == house) {
					houseChanges.push_back(c);
					CHECK(c.via == g.GetDoor(g.FindDoor("houseIn")).doorway);
					if (c.to == Tier::Live) {
						liveAt = x;
					}
				}
			}
		}
		const auto order = TiersOf(houseChanges, house);
		CHECK((order == std::vector<Tier>{ Tier::Image, Tier::DepthImage, Tier::LoadedPaused, Tier::Live }));
		// a picture from far, a depth picture inside the depth reach, loaded before it is the door's turn
		CHECK(houseChanges.size() == 4 && houseChanges[0].from == Tier::Cold && houseChanges[3].from == Tier::LoadedPaused);
		// live only once its door is open and within reach (the reach is 450 from x = 1000, the door opened at x = 400)
		CHECK(liveAt >= 550.0f - 1.0f && liveAt <= 610.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::Live);
		// the player's own space is live from the first tick
		CHECK(walk.scheduler.TierOf(tamriel) == Tier::Live);
		// the locked cave never goes beyond a depth picture; the island has no doorway
		CHECK(walk.scheduler.TierOf(g.FindSpace("cave")) <= Tier::DepthImage);
		// the scripted door's space (broken doorway: tamriel is the player's) and unreachable ones stay cold
		CHECK(walk.scheduler.TierOf(g.FindSpace("island")) == Tier::Cold && walk.scheduler.TierOf(g.FindSpace("room")) == Tier::Cold);
	}

	void SchedulerCloseDoorNotLive()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("tamriel");
		walk.in.position = { 900, 0, 0 };
		walk.Stand(3.0f);
		// beside the door but it is shut: loaded and paused, not simulating
		CHECK(walk.scheduler.TierOf(g.FindSpace("house")) == Tier::LoadedPaused);
		walk.Open("houseIn");
		walk.Stand(0.5f);
		CHECK(walk.scheduler.TierOf(g.FindSpace("house")) == Tier::Live);
		// it shuts again
		walk.open.clear();
		const auto changes = walk.Stand(0.1f);
		CHECK(!changes.empty() && changes[0].to == Tier::LoadedPaused && changes[0].reason == Reason::DoorClosed);
	}

	void SchedulerWalkAway()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto house = g.FindSpace("house");
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("tamriel");
		walk.in.heading = { -1, 0, 0 };
		walk.in.position = { 900, 0, 0 };
		walk.Open("houseIn");
		walk.Stand(4.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::Live);

		// Walking away at 300 units a second: each tier is let go only past its own reach plus the
		// margin (live 450 + 100, loaded 750 + 400, depth image 1000 + 400, picture 4000 + 400), the door's distance being 1000 - x.
		std::vector<std::pair<Tier, float>> seen;
		for (float x = 900.0f; x > -3600.0f; x -= 5.0f) {
			walk.in.position = { x, 0, 0 };
			walk.in.velocity = { -300, 0, 0 };
			for (const auto& c : walk.Step()) {
				if (c.space == house) {
					seen.emplace_back(c.to, 1000.0f - x);
				}
			}
		}
		CHECK(seen.size() == 4);
		if (seen.size() == 4) {
			CHECK(seen[0].first == Tier::LoadedPaused && seen[0].second > 550.0f && seen[0].second < 565.0f);
			CHECK(seen[1].first == Tier::DepthImage && seen[1].second > 1150.0f && seen[1].second < 1165.0f);
			CHECK(seen[2].first == Tier::Image && seen[2].second > 1400.0f && seen[2].second < 1415.0f);
			CHECK(seen[3].first == Tier::Cold && seen[3].second > 4400.0f && seen[3].second < 4415.0f);
		}
	}

	void SchedulerNoFlapping()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("tamriel");
		walk.in.position = { 555, 0, 0 };  // 445 from the door
		walk.Open("houseIn");
		walk.Stand(4.0f);
		CHECK(walk.scheduler.TierOf(g.FindSpace("house")) == Tier::Live);
		// Stepping across the reach (445, 455) back and forth: nothing changes.
		std::size_t changes = 0;
		for (int i = 0; i < 240; ++i) {
			walk.in.position = { i % 2 ? 545.0f : 555.0f, 0, 0 };
			changes += walk.Step().size();
		}
		CHECK(changes == 0);
		// the same around the load reach (748, 750) for a space that is loaded and paused already: it is kept out to 1150
		walk.open.clear();
		walk.in.position = { 400, 0, 0 };
		walk.Stand(1.0f);
		for (int i = 0; i < 240; ++i) {
			walk.in.position = { i % 2 ? 252.0f : 250.0f, 0, 0 };
			changes += walk.Step().size();
		}
		CHECK(changes == 0 && walk.scheduler.TierOf(g.FindSpace("house")) == Tier::LoadedPaused);
	}

	void SchedulerBudget()
	{
		const auto f = Street();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto a = g.FindSpace("a");
		const auto b = g.FindSpace("b");
		const auto c = g.FindSpace("c");
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("street");
		walk.in.position = { 500, 150, 0 };
		walk.in.heading = Vec3{ 500, 150, 0 } / Vec3{ 500, 150, 0 }.Length();  // towards door B
		walk.Stand(6.0f);
		// room for all three: all loaded and paused, one at a time
		CHECK(walk.scheduler.TierOf(a) == Tier::LoadedPaused && walk.scheduler.TierOf(b) == Tier::LoadedPaused && walk.scheduler.TierOf(c) == Tier::LoadedPaused);
		CHECK(walk.scheduler.MemoryMb() > 359.0f && walk.scheduler.MemoryMb() < 361.0f);

		// The budget shrinks to about one loaded room and a depth image: the least likely (C) gives way
		// entirely, then A as far as it must, and B, the likeliest, stays.
		walk.in.memoryBudgetMb = 150.0f;
		const auto changes = walk.Step();
		CHECK(changes.size() == 2);
		CHECK(walk.scheduler.TierOf(b) == Tier::LoadedPaused && walk.scheduler.TierOf(a) == Tier::DepthImage && walk.scheduler.TierOf(c) == Tier::Cold);
		for (const auto& ch : changes) {
			CHECK(ch.reason == Reason::Budget && ch.from == Tier::LoadedPaused && ch.to == (ch.space == c ? Tier::Cold : Tier::DepthImage));
		}
		CHECK(walk.Stand(3.0f).empty());
		// and it stays that way: no promotion past the budget
		CHECK(walk.scheduler.MemoryMb() <= 150.0f);

		// a tighter one still: B alone
		walk.in.memoryBudgetMb = 121.0f;
		walk.Stand(1.0f);
		CHECK(walk.scheduler.TierOf(b) == Tier::LoadedPaused && walk.scheduler.TierOf(a) == Tier::Cold && walk.scheduler.TierOf(c) == Tier::Cold);
		CHECK(walk.scheduler.MemoryMb() <= 121.0f);
	}

	// What the pictures of a room door cost the scheduler follows the picture settings (Pictures'
	// numbers: a room image at 512 is 1344 x 896 texels of 8 bytes, with its mips ~12 MiB; a depth
	// view 668 x 512).
	void PictureCosts()
	{
		const auto fallback = SpaceCost{};
		const auto defaults = SpaceCost::ForPictures(512, 3);
		CHECK(std::abs(defaults.imageMb - 12.3f) < 0.1f && std::abs(defaults.depthMb - 22.7f) < 0.2f);
		CHECK(std::abs(fallback.imageMb - defaults.imageMb) < 0.1f && std::abs(fallback.depthMb - defaults.depthMb) < 0.2f);
		// the depth tier holds the image as well, and each view adds the same
		const auto one = SpaceCost::ForPictures(512, 1);
		const auto two = SpaceCost::ForPictures(512, 2);
		CHECK(one.depthMb > one.imageMb && std::abs((two.depthMb - one.depthMb) - (one.depthMb - one.imageMb)) < 0.01f);
		CHECK(SpaceCost::ForPictures(512, 0).depthMb == SpaceCost::ForPictures(512, 0).imageMb);
		// a quarter of the height is a sixteenth of the memory; double is four times
		CHECK(std::abs(SpaceCost::ForPictures(128, 3).imageMb * 16.0f - defaults.imageMb) < 0.5f);
		CHECK(SpaceCost::ForPictures(1024, 3).imageMb > 3.9f * defaults.imageMb);
		// a street full of doors: at the default budget a few score pictures at most, and a depth image
		// stays within the ladder (more than the image, less than a loaded room)
		CHECK(defaults.Mb(Tier::Image) < defaults.Mb(Tier::DepthImage) && defaults.Mb(Tier::DepthImage) < defaults.Mb(Tier::LoadedPaused));
		// the depth tier ends where the room's parallax stops being worth its memory, within the picture reach
		const SchedulerConfig config;
		CHECK(config.depthReach < config.imageReach && config.depthReach > Selection::NearRadius(config.liveReach));
	}

	void SchedulerOnlyN()
	{
		const auto f = Street();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto a = g.FindSpace("a");
		const auto b = g.FindSpace("b");
		const auto c = g.FindSpace("c");
		const auto countLive = [&](const Walk& a_walk) { return (a_walk.scheduler.TierOf(a) == Tier::Live) + (a_walk.scheduler.TierOf(b) == Tier::Live) + (a_walk.scheduler.TierOf(c) == Tier::Live); };

		// Three open doors, two within reach: one live (the default), the likelier by where the player faces.
		Walk walk(g);
		walk.in.playerSpace = g.FindSpace("street");
		walk.in.position = { 850, 150, 0 };  // A and B 212 away, C 474
		walk.in.heading = { 0, 1, 0 };       // towards B
		for (const auto* name : { "doorA", "doorB", "doorC" }) {
			walk.Open(name);
		}
		walk.Stand(6.0f);
		CHECK(countLive(walk) == 1 && walk.scheduler.TierOf(b) == Tier::Live);
		// the other open door in reach waits loaded and paused, and C's is out of reach
		CHECK(walk.scheduler.TierOf(a) == Tier::LoadedPaused && walk.scheduler.TierOf(c) == Tier::LoadedPaused);
		// turning to face A changes nothing: A is not clearly likelier than B, which is live already
		walk.in.heading = { 0, -1, 0 };
		CHECK(walk.Stand(1.0f).empty() && walk.scheduler.TierOf(b) == Tier::Live);

		// maxLive 2: two
		SchedulerConfig two;
		two.maxLive = 2;
		Walk wide(g, two);
		wide.in.playerSpace = g.FindSpace("street");
		wide.in.position = { 850, 150, 0 };
		wide.in.heading = { 0, 1, 0 };
		for (const auto* name : { "doorA", "doorB", "doorC" }) {
			wide.Open(name);
		}
		wide.Stand(6.0f);
		CHECK(countLive(wide) == 2);

		// No frame time to spare: no space goes live, and one that is live yields when the frame runs over.
		Walk tight(g);
		tight.in.playerSpace = g.FindSpace("street");
		tight.in.position = { 850, 150, 0 };
		tight.in.headroomMs = 2.0f;  // a live space costs 2 ms and wants a reserve besides
		tight.Open("doorA");
		tight.Stand(6.0f);
		CHECK(countLive(tight) == 0 && tight.scheduler.TierOf(a) == Tier::LoadedPaused);
		tight.in.headroomMs = 8.0f;
		tight.Stand(0.2f);
		CHECK(tight.scheduler.TierOf(a) == Tier::Live);
		tight.in.headroomMs = -1.0f;
		const auto yield = tight.Stand(0.1f);
		CHECK(!yield.empty() && yield[0].to == Tier::LoadedPaused && yield[0].reason == Reason::Headroom);
	}

	void SchedulerPolicy()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto house = g.FindSpace("house");
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("tamriel");
		walk.in.position = { 900, 0, 0 };
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::LoadedPaused);

		// A doorway that fell back to the engine's own load holds nothing.
		walk.scheduler.Fallback(g.GetDoor(g.FindDoor("houseIn")).doorway);
		const auto changes = walk.Stand(0.1f);
		CHECK(walk.scheduler.IsFallback(g.GetDoor(g.FindDoor("houseIn")).doorway) && walk.scheduler.TierOf(house) == Tier::Cold && !changes.empty());
		walk.Stand(2.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::Cold);

		// The adapter could not load the inn: held at its picture, not tried again for the retry time.
		const auto inn = g.FindSpace("inn");
		walk.in.position = { 100, 900, 0 };
		walk.Stand(0.25f);
		walk.scheduler.Failed(inn, Tier::DepthImage);
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(inn) <= Tier::DepthImage);
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(inn) == Tier::LoadedPaused);

		// Crossing into the house: it is the player's space, live at once; Tamriel is left behind it.
		walk.in.playerSpace = house;
		walk.in.position = { 0, 0, 0 };
		const auto entered = walk.Step();
		CHECK(!entered.empty() && entered[0].space == house && entered[0].to == Tier::Live && entered[0].reason == Reason::PlayerEntered);
	}

	// A door that is only a door right now holds nothing; reset lets go of everything; a graph built
	// again with more in it keeps the tiers, and the fallbacks, of the spaces and doors it had.
	void SchedulerBlockedResetRebind()
	{
		const auto f = Village();
		const auto g = SpaceGraph::Build(f.spaces, f.doors);
		const auto house = g.FindSpace("house");
		const auto houseDoor = g.FindDoor("houseIn");
		Walk       walk(g);
		walk.in.playerSpace = g.FindSpace("tamriel");
		walk.in.position = { 900, 0, 0 };
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::LoadedPaused);

		// Locked shut right now (the graph says nothing of it): nothing is held for its doorway.
		const std::vector<DoorIx> blocked{ houseDoor };
		walk.in.blockedDoors = blocked;
		walk.Stand(0.1f);
		CHECK(walk.scheduler.TierOf(house) == Tier::Cold);
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::Cold);
		walk.in.blockedDoors = {};
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::LoadedPaused);

		// Everything let go: cold again, and then held again as before.
		walk.scheduler.Reset();
		CHECK(walk.scheduler.TierOf(house) == Tier::Cold && walk.scheduler.MemoryMb() == 0.0f);
		walk.Stand(3.0f);
		CHECK(walk.scheduler.TierOf(house) == Tier::LoadedPaused);

		// The same places and a new one in a graph built again: tiers and the fallback follow by id.
		auto spaces = f.spaces;
		auto doors = f.doors;
		spaces.push_back(Space("cellar", SpaceKind::Interior));
		Pair(doors, "cellarIn", "tamriel", { -500, 0, 0 }, "cellarOut", "cellar", { 0, 0, 0 });
		const auto g2 = SpaceGraph::Build(spaces, doors);
		walk.scheduler.Fallback(g.GetDoor(g.FindDoor("innIn")).doorway);
		walk.scheduler.Rebind(g, g2);
		CHECK(g2.SpaceCount() == g.SpaceCount() + 1);
		CHECK(walk.scheduler.TierOf(g2.FindSpace("house")) == Tier::LoadedPaused);
		CHECK(walk.scheduler.IsFallback(g2.GetDoor(g2.FindDoor("innIn")).doorway) && !walk.scheduler.IsFallback(g2.GetDoor(g2.FindDoor("houseIn")).doorway));
		walk.graph = &g2;
		const auto changes = walk.Stand(1.0f);
		CHECK(TiersOf(changes, g2.FindSpace("house")).empty());  // nothing to do for what it already holds
	}

	// ---- the crossing sequence

	using namespace Core::Crossing;

	bool Has(const std::vector<Action>& a_actions, ActionKind a_kind)
	{
		return std::ranges::any_of(a_actions, [&](const Action& a_action) { return a_action.kind == a_kind; });
	}

	Event Begin()
	{
		return { EventKind::Begin, { .doorway = 3, .door = 7, .from = 1, .to = 2 } };
	}

	void CrossingHappyPath()
	{
		Sequence seq;
		CHECK(!seq.Busy());
		CHECK(seq.Handle(Begin()).empty() && seq.GetPhase() == Phase::Approach);
		const auto prewarm = seq.Handle({ EventKind::Near });
		CHECK(seq.GetPhase() == Phase::Prewarm && prewarm.size() == 1 && prewarm[0].kind == ActionKind::PrewarmSpace && prewarm[0].space == 2);
		const auto open = seq.Handle({ EventKind::Prewarmed });
		CHECK(seq.GetPhase() == Phase::Open && open.size() == 1 && open[0].kind == ActionKind::OpenDoor && open[0].door == 7);
		CHECK(seq.Handle({ EventKind::DoorOpened }).empty() && seq.GetPhase() == Phase::Commit);
		const auto cross = seq.Handle({ EventKind::AtThreshold });
		CHECK(seq.GetPhase() == Phase::Cross && Has(cross, ActionKind::StartCover) && Has(cross, ActionKind::Teleport));
		CHECK(cross.back().kind == ActionKind::Teleport && cross.back().space == 2 && cross.back().door == 7);
		CHECK(Has(seq.Handle({ EventKind::Crossed }), ActionKind::EndCover) && seq.GetPhase() == Phase::Settle);
		const auto release = seq.Handle({ EventKind::Settled });
		CHECK(seq.GetPhase() == Phase::Release && release.size() == 1 && release[0].kind == ActionKind::ReleaseSpace && release[0].space == 1);
		CHECK(seq.Handle({ EventKind::Released }).empty() && seq.GetPhase() == Phase::Idle);
		// events that don't fit are ignored
		CHECK(seq.Handle({ EventKind::Crossed }).empty() && seq.Handle({ EventKind::Prewarmed }).empty() && seq.GetPhase() == Phase::Idle);
	}

	void CrossingFallbackAndAbort()
	{
		// Prewarm does not finish: the engine's own load, with its cover, never a hang.
		Sequence seq;
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		CHECK(seq.Tick(5.0f).empty() && seq.GetPhase() == Phase::Prewarm);
		const auto fallback = seq.Tick(1.5f);
		CHECK(seq.GetPhase() == Phase::Fallback && Has(fallback, ActionKind::CancelPrewarm) && Has(fallback, ActionKind::VanillaDoor) && Has(fallback, ActionKind::StartCover));
		CHECK(fallback[1].cover == Cover::EngineLoad);
		CHECK(Has(seq.Handle({ EventKind::Crossed }), ActionKind::EndCover) && seq.GetPhase() == Phase::Idle);

		// An adapter failure falls back too; the same at the cross itself.
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		CHECK(Has(seq.Handle({ EventKind::Failed }), ActionKind::VanillaDoor) && seq.GetPhase() == Phase::Fallback);
		(void)seq.Tick(11.0f);
		CHECK(seq.GetPhase() == Phase::Idle);
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		(void)seq.Handle({ EventKind::Prewarmed });
		(void)seq.Handle({ EventKind::DoorOpened });
		(void)seq.Handle({ EventKind::AtThreshold });
		CHECK(seq.GetPhase() == Phase::Cross && Has(seq.Tick(2.5f), ActionKind::VanillaDoor) && seq.GetPhase() == Phase::Fallback);
		(void)seq.Handle({ EventKind::Crossed });

		// The player walks off before the cross: back to idle, the prewarm cancelled; after the cross there is no abort.
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		CHECK(Has(seq.Handle({ EventKind::Abort }), ActionKind::CancelPrewarm) && seq.GetPhase() == Phase::Idle);
		(void)seq.Handle(Begin());
		CHECK(seq.Handle({ EventKind::Abort }).empty() && seq.GetPhase() == Phase::Idle);
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		(void)seq.Handle({ EventKind::Prewarmed });
		(void)seq.Handle({ EventKind::DoorOpened });
		(void)seq.Handle({ EventKind::AtThreshold });
		CHECK(seq.Handle({ EventKind::Abort }).empty() && seq.GetPhase() == Phase::Cross);
		(void)seq.Handle({ EventKind::Crossed });
		// a player lingering at the open door lets go after a while, not a failure
		(void)seq.Handle({ EventKind::Settled });
		(void)seq.Handle({ EventKind::Released });
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		(void)seq.Handle({ EventKind::Prewarmed });
		(void)seq.Handle({ EventKind::DoorOpened });
		CHECK(Has(seq.Tick(16.0f), ActionKind::CancelPrewarm) && seq.GetPhase() == Phase::Idle);
	}

	void CrossingRushedAndTimed()
	{
		// The player is at the threshold before the door is open: crosses the moment it is.
		Sequence seq;
		(void)seq.Handle(Begin());
		(void)seq.Handle({ EventKind::Near });
		(void)seq.Handle({ EventKind::AtThreshold });
		(void)seq.Handle({ EventKind::Prewarmed });
		CHECK(seq.GetPhase() == Phase::Open);
		const auto go = seq.Handle({ EventKind::DoorOpened });
		CHECK(seq.GetPhase() == Phase::Cross && Has(go, ActionKind::Teleport));
		(void)seq.Handle({ EventKind::Crossed });
		// Settle and release finish by themselves if the adapter does not report.
		CHECK(seq.GetPhase() == Phase::Settle);
		CHECK(Has(seq.Tick(1.1f), ActionKind::ReleaseSpace) && seq.GetPhase() == Phase::Release);
		(void)seq.Tick(2.1f);
		CHECK(seq.GetPhase() == Phase::Idle);
	}
}

int RunGraphTests()
{
	Run("graph from a fixture: doorways, neighbours, paths, radius", GraphFixture);
	Run("graph from the door census (skipped without the file)", CensusGraph);
	Run("scheduler: walking up promotes one tier at a time", SchedulerApproach);
	Run("scheduler: a shut door is not live", SchedulerCloseDoorNotLive);
	Run("scheduler: walking away demotes past the hysteresis", SchedulerWalkAway);
	Run("scheduler: no flapping at a boundary", SchedulerNoFlapping);
	Run("scheduler: memory budget demotes the least likely", SchedulerBudget);
	Run("scheduler: picture costs follow the picture settings", PictureCosts);
	Run("scheduler: only N live, headroom", SchedulerOnlyN);
	Run("scheduler: fallback, failure, player entering", SchedulerPolicy);
	Run("scheduler: blocked doors, reset, rebind", SchedulerBlockedResetRebind);
	Run("crossing sequence: happy path", CrossingHappyPath);
	Run("crossing sequence: fallback, abort, timeouts", CrossingFallbackAndAbort);
	Run("crossing sequence: rushed threshold, settle", CrossingRushedAndTimed);
	return failures;
}
