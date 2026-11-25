// Unit tests of the game-independent Core: the maths that moved out of the Skyrim modules, run
// without a game. Plain checks (assert is compiled out in the build types that matter here).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "Core/Crossing/Approach.h"
#include "Core/Doors/Mapping.h"
#include "Core/Doors/Panel.h"
#include "Core/Doors/Policy.h"
#include "Core/Doors/Swing.h"
#include "Core/ImageSpace/Blend.h"
#include "Core/Pictures/CacheRules.h"
#include "Core/Pictures/Camera.h"
#include "Core/Render/DoorwayPlanes.h"
#include "Core/Selection/Ranking.h"

namespace
{
	int failures = 0;

#define CHECK(a_condition)                                                              \
	do {                                                                                \
		if (!(a_condition)) {                                                           \
			std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #a_condition);       \
			++failures;                                                                 \
		}                                                                               \
	} while (false)

	bool Near(float a, float b, float a_tolerance = 1e-3f)
	{
		return std::abs(a - b) <= a_tolerance;
	}

	bool Near(const Core::Vec3& a, const Core::Vec3& b, float a_tolerance = 1e-2f)
	{
		return Near(a.x, b.x, a_tolerance) && Near(a.y, b.y, a_tolerance) && Near(a.z, b.z, a_tolerance);
	}

	void Run(const char* a_name, const std::function<void()>& a_test)
	{
		const int before = failures;
		a_test();
		std::printf("%s %s\n", failures == before ? "ok    " : "FAILED", a_name);
	}

	// A door on a street facing +y (its panel's normal points along y), the marker where you stand
	// to come out of it.
	Core::Doors::PairDoor Door(Core::Vec3 a_center, float a_thickness, float a_markerYaw, float a_halfHeight = 60.0f, bool a_outdoors = true)
	{
		return { a_center, { 0.0f, a_thickness, 0.0f }, a_markerYaw, 50.0f, a_thickness * 0.5f, a_halfHeight, a_outdoors };
	}

	void MappingRoundTrip()
	{
		using namespace Core;
		// A city gate placed at two sizes (10% larger on the far side): the two gateways are made one
		// by scaling, so a point of the source side comes out at the matching place on the other.
		const auto source = Door({ 100.0f, 200.0f, 10.0f }, 8.0f, 0.0f, 60.0f);
		const auto linked = Door({ -3000.0f, 500.0f, 30.0f }, 8.0f, kPi, 66.0f);
		const auto mapping = Doors::MapPair(source, linked, true, {});
		CHECK(Near(mapping.toLinked.scale, 1.1f));
		CHECK(Near(mapping.toLinked * source.center, linked.center));
		const Vec3 probe{ 130.0f, 150.0f, 44.0f };
		CHECK(Near(mapping.toLinked.Invert() * (mapping.toLinked * probe), probe));
		CHECK(Near(mapping.sourceCenter, source.center));
		CHECK(Near(mapping.inward.Length(), 1.0f));
		CHECK(Near(mapping.inward.z, 0.0f));

		// Different models: no scaling, only the rigid map.
		const auto rigid = Doors::MapPair(source, linked, false, {});
		CHECK(Near(rigid.toLinked.scale, 1.0f));
		// Indoors on one side: no scaling either.
		auto inside = linked;
		inside.outdoors = false;
		CHECK(Near(Doors::MapPair(source, inside, true, {}).toLinked.scale, 1.0f));
		// Within 1% it is the measurement's noise.
		auto near = linked;
		near.halfHeight = 60.4f;
		CHECK(Near(Doors::MapPair(source, near, true, {}).toLinked.scale, 1.0f));
	}

	void MappingCorrection()
	{
		using namespace Core;
		const auto source = Door({ 0.0f, 0.0f, 0.0f }, 8.0f, 0.0f);
		const auto linked = Door({ 1000.0f, 0.0f, 0.0f }, 8.0f, kPi);
		const auto plain = Doors::MapPair(source, linked, false, {});
		// A correction held by the source door turns the inside around its own door: the heading
		// the mapping turns by changes by the corrected angle, and the doorway's centre stays put.
		const auto turned = Doors::MapPair(source, linked, false, { .degrees = 30.0f, .keyIsSource = true });
		float      delta = turned.yawDelta - plain.yawDelta;
		while (delta > kPi) {
			delta -= 2.0f * kPi;
		}
		while (delta < -kPi) {
			delta += 2.0f * kPi;
		}
		CHECK(Near(std::abs(delta), DegToRad(30.0f), 1e-3f));
		CHECK(Near(turned.toLinked * source.center, linked.center));
		// From the other door the way back turns the opposite way, and is the inverse of the way there
		// where the pair is symmetric.
		const auto back = Doors::MapPair(linked, source, false, { .degrees = 30.0f, .keyIsSource = false });
		CHECK(Near(back.toLinked * linked.center, source.center));
		// A shift moves the picture along the key door's space.
		const auto shifted = Doors::MapPair(source, linked, false, { .shift = { 0.0f, 10.0f, 0.0f }, .keyIsSource = true });
		CHECK(!Near(shifted.toLinked.translate, plain.toLinked.translate));
		CHECK(Near(shifted.yawDelta, plain.yawDelta));
	}

	void SwingTiming()
	{
		using namespace Core::Doors;
		// A door that takes 2 s to open and 1 s to shut, linear.
		const auto linear = [](float seconds) {
			return SampleCurve(seconds, [](float t) { return t; });
		};
		const SwingCurves curves{ linear(2.0f), linear(1.0f) };

		SwingState swing;
		CHECK(swing.Shut());
		CHECK(swing.Step(curves, true, 0.5f));  // started out shut
		CHECK(Near(swing.Amount(), 0.25f));
		CHECK(!swing.Step(curves, true, 0.5f));
		CHECK(Near(swing.Amount(), 0.5f));
		swing.Step(curves, true, 10.0f);
		CHECK(swing.Amount() == 1.0f);  // clamped
		swing.Step(curves, false, 0.5f);
		CHECK(Near(swing.Amount(), 0.5f));  // shutting takes 1 s
		swing.Step(curves, false, 10.0f);
		CHECK(swing.Shut());

		// Turned round half way on eased curves the progress is taken from where the other
		// direction's curve is as far open: no jump in how far open the door is.
		const SwingCurves eased{ StillCurve(true), StillCurve(false) };
		SwingState        turn;
		turn.Step(eased, true, 0.3f);
		const float openBefore = eased.Amount(turn.Amount(), true);
		turn.Step(eased, false, 0.0f);
		CHECK(Near(eased.Amount(turn.Amount(), false), openBefore, 5e-3f));
		CHECK(!turn.Opening());

		// A door handed from one owner to the next is taken over where it was left.
		LetGoBook book;
		SwingState first;
		first.Step(curves, true, 1.0f);
		book.LetGo(0x1234, first);
		SwingState second;
		CHECK(book.TakeOver(0x1234, second));
		CHECK(Near(second.Amount(), 0.5f));
		CHECK(second.Opening());
		CHECK(!book.TakeOver(0x1234, second));  // taken once
		CHECK(second.Shut());                   // and shut for a door nobody let go of

		// Let go of for good after kSeconds, unless reclaimed.
		book.LetGo(1, first);
		book.LetGo(2, first);
		book.Reclaim(2);
		CHECK(book.Expire(2.0f).empty());
		const auto expired = book.Expire(1.5f);
		CHECK(expired.size() == 1 && expired[0] == 1);
		CHECK(book.Expire(10.0f).empty());
		book.LetGo(7, first);
		CHECK(book.TakeAll().size() == 1);
	}

	void PolicyTable()
	{
		using namespace Core::Doors;
		const auto none = [] { return false; };
		const auto yes = [] { return true; };
		int        asked = 0;
		const auto picture = [&] {
			++asked;
			return true;
		};

		PairFacts f;
		f.doorOutdoors = true;
		f.linkedOutdoors = true;
		// A gate: outdoors on both sides stands open from afar, without asking for a picture.
		CHECK(OpenFromAfar(f, picture));
		CHECK(asked == 0);
		CHECK(LockedShut(true, Force::kNone) && LockedShut(true, Force::kAllShut) && !LockedShut(true, Force::kAllOpen) && !LockedShut(false, Force::kNone));
		// Forced open or shut wins over everything.
		f.force = Force::kAllOpen;
		f.doorOutdoors = false;
		CHECK(OpenFromAfar(f, none));
		f.force = Force::kAllShut;
		f.doorOutdoors = true;
		CHECK(!OpenFromAfar(f, yes));
		CHECK(!StandsOpen(f, yes, yes));
		f.alwaysOpen = true;
		CHECK(!StandsOpen(f, yes, yes));  // forced shut: not even the per-door switch
		// Used doors are wanted even when forced shut.
		f.doorUsed = true;
		CHECK(Wanted(f, none, none));
		f.doorUsed = false;
		f.linkedUsed = true;
		CHECK(Wanted(f, none, none));

		// An ordinary door pair: shut until used.
		PairFacts shop;
		shop.doorOutdoors = true;
		shop.linkedOutdoors = false;
		CHECK(!OpenFromAfar(shop, yes));
		CHECK(!StandsOpen(shop, yes, yes));
		CHECK(!Wanted(shop, yes, yes));
		shop.doorUsed = true;
		CHECK(Wanted(shop, none, none));
		shop.doorUsed = false;
		// Set to stand open always: from afar once it has a picture, otherwise when its space is shown.
		shop.alwaysOpen = true;
		CHECK(!OpenFromAfar(shop, none));
		CHECK(OpenFromAfar(shop, yes));
		CHECK(!StandsOpen(shop, none, none));
		CHECK(StandsOpen(shop, none, yes));
		CHECK(Wanted(shop, none, yes));
		// A door out of a building shows the street live only: never open from afar by a picture.
		PairFacts out;
		out.doorOutdoors = false;
		out.linkedOutdoors = true;
		out.alwaysOpen = true;
		CHECK(StreetShownLive(out.doorOutdoors, out.linkedOutdoors));
		asked = 0;
		CHECK(!OpenFromAfar(out, picture));
		CHECK(asked == 0);
		CHECK(!StandsOpen(out, picture, none));
		CHECK(StandsOpen(out, picture, yes));  // opens once its street is shown
		// Interior to interior.
		PairFacts rooms;
		rooms.alwaysOpen = true;
		CHECK(OpenFromAfar(rooms, yes));
		CHECK(!OpenFromAfar(rooms, none));
	}

	void RankHysteresis()
	{
		using namespace Core;
		namespace S = Core::Selection;
		// A door ahead ranks up to half the switch margin better than the same distance behind.
		const Vec3 ahead{ 0.0f, 500.0f, 0.0f };
		const Vec3 heading{ 0.0f, 1.0f, 0.0f };
		CHECK(Near(S::Rank(500.0f, heading, ahead), 500.0f - S::kHeadingNudge));
		CHECK(Near(S::Rank(500.0f, heading, ahead * -1.0f), 500.0f + S::kHeadingNudge));
		CHECK(Near(S::Rank(500.0f, {}, ahead), 500.0f));  // no heading yet
		// Turning round between two doors equally far changes their order by twice the nudge, which
		// is less than the margin a door must be nearer by to take over.
		CHECK(2.0f * S::kHeadingNudge <= S::kSwitchMargin);
		// A city gate counts nearer; a house door walked up to near a gate still wins.
		CHECK(S::Reach(1000.0f, true) == 250.0f);
		CHECK(S::Reach(1000.0f, false) == 1000.0f);
		CHECK(S::Reach(100.0f, false) < S::Reach(1000.0f, true) + S::kSwitchMargin);

		// The heading follows only real steps, and a jump is no direction.
		S::HeadingTracker tracker;
		tracker.Track({ 10.0f, 0.0f, 0.0f });
		CHECK(tracker.Heading() == Vec3());  // too short a step
		tracker.Track({ 0.0f, 100.0f, 0.0f });
		CHECK(Near(tracker.Heading(), { 0.0f, 1.0f, 0.0f }));
		tracker.Track({ 0.0f, 3000.0f, 0.0f });
		CHECK(tracker.Heading() == Vec3());  // a teleport

		// The crowd is sparse below `few` spaces and keeps its last answer in between.
		auto crowd = S::Assess(false, true, 2, 3, 3, false, 4000.0f);
		CHECK(crowd.sparse);
		CHECK(crowd.warmReach == 4000.0f);
		CHECK(crowd.farActivate == 0.0f);  // outdoors: only a door into a world widens the shown reach
		crowd = S::Assess(true, true, 2, 5, 3, true, 4000.0f);
		CHECK(crowd.sparse);  // between: unchanged
		CHECK(crowd.farActivate == 4000.0f);
		crowd = S::Assess(true, true, 4, 5, 3, false, 4000.0f);
		CHECK(!crowd.sparse);
		CHECK(crowd.warmReach == S::kWarmDistance);
		crowd = S::Assess(false, true, 4, 5, 3, false, 4000.0f);
		CHECK(!crowd.sparse);  // between, not sparse: unchanged
		CHECK(S::Assess(false, false, 1, 1, 3, false, 2000.0f).farActivate == 2000.0f);  // indoors one space is loaded early

		// Warm targets: a room with two doors goes by the better rank but is shown through the nearer door.
		S::WarmTargets targets;
		targets.Note({ 0xA, 900.0f, 880.0f, 1, 2 });
		targets.Note({ 0xB, 300.0f, 350.0f, 3, 4 });
		targets.Note({ 0xA, 600.0f, 910.0f, 5, 6 });
		targets.Sort();
		CHECK(targets.Count() == 2);
		const auto* a = targets.Find(0xA);
		CHECK(a && a->rank == 880.0f && a->distance == 600.0f && a->door == 5 && a->linked == 6);
		CHECK(targets.RankOf(0xC) == FLT_MAX);
		const auto wanted = targets.Wanted(2000.0f, 0, 3);
		CHECK(wanted.size() == 2 && wanted[0] == 0xB && wanted[1] == 0xA);
		// Room for one beside the shown space only, and not the shown space's own cell.
		CHECK(targets.Wanted(2000.0f, 0xB, 2) == std::vector<std::uint32_t>{ 0xA });
		CHECK(targets.Wanted(500.0f, 0, 3) == std::vector<std::uint32_t>{ 0xB });

		// A kept room makes way only for a door clearly nearer than its own.
		CHECK(!S::MakesWayFor(400.0f, 500.0f));
		CHECK(S::MakesWayFor(200.0f, 500.0f));
		// A cell that failed to attach waits.
		S::AttachRetry retry;
		retry.Failed(0xA);
		CHECK(retry.Blocks(0xA) && !retry.Blocks(0xB));
		retry.Tick(S::kRetryCooldown + 0.1f);
		CHECK(!retry.Blocks(0xA));
	}

	void CrossingApproach()
	{
		using namespace Core;
		namespace C = Core::Crossing;
		C::Timers timers;
		CHECK(timers.Ready() && !timers.AttachHeld());
		timers.CrossingRequested();
		CHECK(!timers.Ready() && timers.AttachHeld());
		timers.Tick(1.5f);
		CHECK(timers.Ready() && timers.AttachHeld());
		timers.Tick(2.0f);
		CHECK(!timers.AttachHeld());
		timers.HoldAttach(2.0f);
		timers.HoldAttach(1.0f);  // never shorter
		timers.Tick(1.5f);
		CHECK(timers.AttachHeld());

		// A door in the plane y = 0, inward +y; the player walks up from y = -200 (depth -200).
		C::Approach in;
		in.door = 0x42;
		in.doorway = { { 0.0f, 0.0f, 100.0f }, { 0.0f, 1.0f, 0.0f }, 50.0f, 4.0f, 60.0f };
		in.player = { 0.0f, -200.0f, 40.0f };
		in.camera = { 0.0f, -230.0f, 100.0f };
		in.delta = 0.016f;
		in.pullIn = true;
		in.playerMoving = true;
		in.playerYaw = 0.0f;  // forward = (sin, cos) = +y

		C::ApproachState state;
		auto             d = state.Decide(in);
		CHECK(d.active && !d.cross);
		CHECK(Near(d.depth, -200.0f));
		CHECK(d.cameraLimit && Near(*d.cameraLimit, 200.0f - (4.0f + 4.0f)));
		in.player.y = -9.0f;  // within the lead (kCrossLead + halfDepth = 8): not yet; then walking nearer
		in.camera.y = -20.0f;
		d = state.Decide(in);
		CHECK(!d.cross);
		in.player.y = -7.0f;
		d = state.Decide(in);
		CHECK(d.cross);  // approaching and within the lead
		// The same spot with no approach (just arrived and standing there) carries nobody through.
		C::ApproachState fresh;
		d = fresh.Decide(in);
		CHECK(!d.cross);
		d = fresh.Decide(in);  // still standing
		CHECK(!d.cross);
		// Off the doorway's height or far to the side: not at the doorway, and a passage is closed.
		in.passageOpen = true;
		in.player.z = 400.0f;
		d = state.Decide(in);
		CHECK(!d.active && d.closePassage);
		in.player.z = 40.0f;
		in.player.x = 500.0f;
		d = state.Decide(in);
		CHECK(!d.active && d.closePassage);
		// Merged: crosses only well through the doorway.
		C::ApproachState merged;
		C::Approach inMerged = in;
		inMerged.passageOpen = false;
		inMerged.player = { 0.0f, 20.0f, 40.0f };
		inMerged.camera = { 0.0f, 3.0f, 100.0f };
		inMerged.merged = true;
		CHECK(merged.Decide(inMerged).cross);
		inMerged.camera.y = 1.0f;
		CHECK(!merged.Decide(inMerged).cross);
		inMerged.player.y = 61.0f;
		CHECK(merged.Decide(inMerged).cross);
		// Picture depth: behind the panel for a door, in front for a gate.
		CHECK(C::PictureDepth(4.0f, false) == 6.0f && C::PictureDepth(4.0f, true) == -6.0f);
	}

	void PanelAndPlanes()
	{
		using namespace Core;
		namespace D = Core::Doors;
		// A panel 100 wide (x), 8 thick (y), 120 high; faces at y = 0 and y = 8.
		D::Box panel;
		panel.Add({ -50.0f, 0.0f, 0.0f });
		panel.Add({ 50.0f, 8.0f, 120.0f });
		CHECK(panel.ThinAlongX() == false);
		const auto size = D::SizeOf(panel, false, 1.0f);
		CHECK(Near(size.halfWidth, 50.0f) && Near(size.halfDepth, 4.0f) && Near(size.halfHeight, 60.0f));
		const auto corners = D::BoxCorners(panel, {});
		// Seen from -y the quad is 2 units in front of the min face; from +y, in front of the max face.
		auto quad = D::QuadOnFace(corners, false, { 0.0f, -100.0f, 60.0f }, false);
		CHECK(Near(quad[0].y, -2.0f) && Near(quad[3].y, -2.0f));
		quad = D::QuadOnFace(corners, false, { 0.0f, 100.0f, 60.0f }, false);
		CHECK(Near(quad[0].y, 10.0f));
		quad = D::QuadOnFace(corners, false, { 0.0f, -100.0f, 60.0f }, true);  // behind the panel
		CHECK(Near(quad[0].y, 10.0f));
		// A door standing sideways in the model is stood up by quarter turns.
		const auto upright = D::UprightRotation({ 0.0f, 1.0f, 0.0f });
		CHECK(Near(upright * Vec3{ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }));
		CHECK(Near(D::UprightRotation({ 0.0f, 0.0f, 1.0f }) * Vec3{ 1.0f, 2.0f, 3.0f }, { 1.0f, 2.0f, 3.0f }));
		// The leaf turns about its hinge towards the room, by a_amount of the opening angle.
		D::LeafSwing leaf{ panel, 0.0f, true };  // the root: hinged at its low end (x = -50)
		const auto   open = D::LeafTurn(leaf, false, { 0.0f, 1.0f, 0.0f }, 1.0f, 90.0f);
		CHECK(Near(open * Vec3{ -50.0f, 4.0f, 0.0f }, { -50.0f, 4.0f, 0.0f }));  // the hinge stays
		CHECK(Near((open * Vec3{ 50.0f, 4.0f, 0.0f }).y, 104.0f, 0.05f));        // the free end swung into +y
		const auto shut = D::LeafTurn(leaf, false, { 0.0f, 1.0f, 0.0f }, 0.0f, 90.0f);
		CHECK(Near(shut * Vec3{ 50.0f, 4.0f, 0.0f }, { 50.0f, 4.0f, 0.0f }));
		// The faces are found where the surface is.
		std::vector<D::Triangle> triangles;
		const auto               wall = [&](float y) {
            triangles.push_back({ { -50.0f, y, 0.0f }, { 50.0f, y, 0.0f }, { 50.0f, y, 120.0f }, 0 });
            triangles.push_back({ { -50.0f, y, 0.0f }, { 50.0f, y, 120.0f }, { -50.0f, y, 120.0f }, 0 });
		};
		wall(0.5f);
		wall(7.5f);
		const auto faces = D::FindFaces(triangles, false, 1);
		CHECK(faces.Found() && faces.front == 0 && faces.back == 7);
		D::Box clamped;
		clamped.Add({ -50.0f, -3.0f, 0.0f });
		clamped.Add({ 50.0f, 12.0f, 120.0f });
		D::ClampToFaces(clamped, false, faces);
		CHECK(Near(clamped.min.y, 0.0f) && Near(clamped.max.y, 8.0f));
		// A shape behind the panel (seen from the marker side at y < 0) and one in front of it.
		D::Box behind, frame;
		behind.Add({ -40.0f, 6.0f, 0.0f });
		behind.Add({ 40.0f, 30.0f, 100.0f });
		frame.Add({ -60.0f, -5.0f, 0.0f });
		frame.Add({ 60.0f, 14.0f, 130.0f });
		CHECK(D::IsBehindPanel(behind, panel, false, { 0.0f, -20.0f, 0.0f }));   // reaches out past the far face
		CHECK(!D::IsBehindPanel(behind, panel, false, { 0.0f, 20.0f, 0.0f }));   // the other side's view of it: in front
		CHECK(!D::IsBehindPanel(frame, panel, false, { 0.0f, -20.0f, 0.0f }));  // a frame reaches out both ways
		CHECK(!D::IsBehindPanel(frame, panel, false, { 0.0f, 20.0f, 0.0f }));

		// The doorway planes: a camera in front of a door sees through its rectangle only.
		namespace P = Core::Doorway;
		const auto door = Render::DoorRect::Upright({ 0.0f, 0.0f, 100.0f }, { 0.0f, 1.0f, 0.0f }, 50.0f, 60.0f, false);
		P::Plane   planes[5];
		CHECK(P::Planes(door, { 0.0f, -200.0f, 100.0f }, planes));
		const auto inside = [&](const Vec3& a_point) {
			for (const auto& plane : planes) {
				if (plane.At(a_point) < 0.0) {
					return false;
				}
			}
			return true;
		};
		CHECK(inside({ 0.0f, 500.0f, 100.0f }));    // straight through the doorway
		CHECK(!inside({ 0.0f, 2.0f, 100.0f }));    // within the margin of the door plane
		CHECK(!inside({ 400.0f, 100.0f, 100.0f }));  // beside it
		CHECK(!inside({ 0.0f, -100.0f, 100.0f }));   // on the camera's side
		CHECK(!P::Planes(door, { 0.0f, 200.0f, 100.0f }, planes));  // the camera behind the door plane
		CHECK(P::SideOf(planes[0], { 0.0f, 500.0f, 100.0f }, 1.0f) == P::Side::kInside || P::SideOf(planes[0], { 0.0f, 500.0f, 100.0f }, 1.0f) == P::Side::kCrosses);
	}

	void Rules()
	{
		using namespace Core;
		namespace I = Core::ImageSpace;
		namespace P = Core::Pictures;
		// Blend weights: outdoors alone far outside, the interior's alone deep inside, smooth between;
		// beside the doorway the player's own space applies.
		CHECK(I::Weight(-200.0f, 1.0f, false) == 0.0f);
		CHECK(I::Weight(200.0f, 1.0f, false) == 1.0f);
		CHECK(I::Weight(200.0f, 0.0f, false) == 0.0f);
		CHECK(Near(I::Weight(-15.0f, 1.0f, false), 0.5f, 0.01f));
		CHECK(I::Weight(-200.0f, 0.0f, true) == 1.0f);
		CHECK(I::Settled(1.0f, true) && I::Settled(0.0f, false) && !I::Settled(0.5f, true));
		const Crossing::Doorway doorway{ { 0.0f, 0.0f, 100.0f }, { 0.0f, 1.0f, 0.0f }, 50.0f, 4.0f, 60.0f };
		CHECK(I::InDoorway(doorway, { 0.0f, -50.0f, 100.0f }) == 1.0f);
		CHECK(I::InDoorway(doorway, { 500.0f, -50.0f, 100.0f }) == 0.0f);
		CHECK(Near(I::InDoorway(doorway, { 100.0f, -50.0f, 100.0f }), 0.5f));
		CHECK(I::InteriorDepth(doorway, { 0.0f, 30.0f, 100.0f }, false) == 30.0f);
		CHECK(I::InteriorDepth(doorway, { 0.0f, 30.0f, 100.0f }, true) == -30.0f);
		float       blend[4]{}, out[4] = { 0, 10, 20, 30 }, in[4] = { 10, 20, 40, 60 };
		const std::size_t keep[] = { 1 };
		I::Mix(blend, out, in, 0.5f, keep);
		CHECK(blend[0] == 5.0f && blend[1] == 0.0f && blend[2] == 30.0f && blend[3] == 45.0f);
		CHECK(I::OutdoorsHolds(true, false, 99.0f) && I::OutdoorsHolds(true, true, 0.4f) && !I::OutdoorsHolds(true, true, 0.6f) && !I::OutdoorsHolds(false, true, 0.0f));
		CHECK(I::WeatherTimeOf(4.9f) == I::WeatherTime::kNight && I::WeatherTimeOf(6.0f) == I::WeatherTime::kSunrise && I::WeatherTimeOf(12.0f) == I::WeatherTime::kDay &&
			  I::WeatherTimeOf(19.0f) == I::WeatherTime::kSunset);

		// The picture cache: keys by door and part of the day, valid headers, the oldest let go.
		CHECK(P::PartOfDay(2.0f) == 0 && P::PartOfDay(6.0f) == 1 && P::PartOfDay(12.0f) == 2 && P::PartOfDay(19.0f) == 3 && P::PartOfDay(21.0f) == 0);
		const auto order = P::SearchOrder(2);
		CHECK(order[0] == 2 && order[1] == 3 && order[2] == 1 && order[3] == 0);
		CHECK(P::DoorKey("Skyrim.esm", 0x0001A6FB, false) == "Skyrim.esm|01A6FB");
		CHECK(P::DoorKey("Light.esl", 0xFE000ABC, true) == "Light.esl|000ABC");
		CHECK(P::KeyOf("Skyrim.esm|01A6FB", true, 2) == "Skyrim.esm|01A6FB#day");
		CHECK(P::KeyOf("Skyrim.esm|01A6FB", false, 2) == "Skyrim.esm|01A6FB#inside");
		CHECK(P::FileStem("Skyrim.esm|01A6FB#day") == "Skyrim.esm_01A6FB_day");
		const std::uint32_t good[4] = { P::kMagic, P::kVersion, 512, 256 };
		CHECK(P::HeaderValid(good, 1024, 512));
		const std::uint32_t tooWide[4] = { P::kMagic, P::kVersion, 4096, 256 };
		const std::uint32_t newer[4] = { P::kMagic, P::kVersion + 1, 512, 256 };
		const std::uint32_t foreign[4] = { 0, P::kVersion, 512, 256 };
		CHECK(!P::HeaderValid(tooWide, 1024, 512) && !P::HeaderValid(newer, 1024, 512) && !P::HeaderValid(foreign, 1024, 512));
		CHECK(!P::HasDepth(1) && P::HasDepth(2));
		P::BrightnessMean mean;
		CHECK(mean.Result(1.0f) == 0.0f);
		mean.Add(1.0f);
		mean.Add(4.0f);
		mean.Add(0.0f);  // black counts as the darkest, not zero
		CHECK(Near(mean.Result(2.0f), std::cbrt(1.0f * 4.0f * P::BrightnessMean::kDarkest) / 2.0f, 1e-4f));
		CHECK(Near(P::GeometricMean({ 1.0f, 4.0f }), 2.0f) && P::GeometricMean({}) == 0.0f);
		CHECK(P::ShouldForget(P::kKeptUnused, false) && !P::ShouldForget(P::kKeptUnused, true) && !P::ShouldForget(P::kKeptUnused / 2, false));
	}

	void PictureCamera()
	{
		namespace C = Core::Pictures::Camera;
		// A doorway 100 wide and 200 high in the plane y = 0, seen from y = -300.
		const Core::Quad corners{ Core::Vec3{ 0, 0, 0 }, Core::Vec3{ 100, 0, 0 }, Core::Vec3{ 0, 0, 200 }, Core::Vec3{ 100, 0, 200 } };
		const Core::Vec3 eye{ 50, -300, 100 };
		const C::Frame   frame(corners, eye);
		CHECK(Near(frame.inward, { 0, 1, 0 }) || Near(frame.inward, { 0, -1, 0 }));
		// Its normal turned away from the eye: the room behind the door is the positive side.
		CHECK(frame.inward.Dot(corners[0] - eye) > 0.0f);
		CHECK(Near(frame.PlaceOf(frame.PointAt({ 0.25f, 0.5f, 30.0f }))[0], 0.25f) && Near(frame.PlaceOf(frame.PointAt({ 0.25f, 0.5f, 30.0f }))[2], 30.0f));
		CHECK(Near(C::AspectOf(corners), 0.75f) && Near(C::AspectOf({ Core::Vec3{}, Core::Vec3{ 600, 0, 0 }, Core::Vec3{ 0, 0, 100 }, Core::Vec3{} }), C::kWidest));
		const auto [width, height] = C::SizeFor(C::kDoorwayOnly, 192, 0.75f);
		CHECK(width == 144 && height == 192 && C::WidthFor(192) == 144);
		// The capture camera's image spans exactly its extent of the door's plane: the extent's corners
		// land on the corners of clip space.
		const auto& shape = C::kRoomCamera;
		const auto  camera = C::For(frame, shape, C::PlaceOf(shape, 0.0f));
		const auto  clip = C::Clip(camera, frame, shape.extent[0], shape.extent[2]);
		const auto  opposite = C::Clip(camera, frame, shape.extent[1], shape.extent[3]);
		CHECK(Near(std::abs(clip[0]), 1.0f) && Near(std::abs(clip[1]), 1.0f) && Near(clip[0], -opposite[0]) && Near(clip[1], -opposite[1]));
	}
}

int main()
{
	Run("door pair mapping round trip", MappingRoundTrip);
	Run("door pair mapping corrections", MappingCorrection);
	Run("swing timing", SwingTiming);
	Run("open-door policy table", PolicyTable);
	Run("selection rank and hysteresis", RankHysteresis);
	Run("crossing approach and timers", CrossingApproach);
	Run("panel, leaf swing and doorway planes", PanelAndPlanes);
	Run("blend weights and picture cache rules", Rules);
	Run("picture camera geometry", PictureCamera);
	std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
	return failures ? 1 : 0;
}
