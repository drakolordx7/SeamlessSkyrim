// Included inside CoreTests.cpp's anonymous namespace (it uses its CHECK and Near).

void DoorwayDaylight()
{
	namespace D = Core::Daylight;
	using Core::Vec3;

	// A doorway 100 wide and 200 high in the plane y = 0, the street at y < 0, the room at y > 0.
	const auto opening = D::Opening::Upright({ 0.0f, 0.0f, 100.0f }, { 0.0f, 1.0f, 0.0f }, 50.0f, 100.0f);
	CHECK(Near(opening.right, { 1.0f, 0.0f, 0.0f }));

	// The sun high in the south-west of the street: toward it is out of the street and up.
	Vec3 toward{ 0.0f, -0.6f, 0.8f };
	toward.Unitize();
	// A floor point straight along the sun's ray from the middle of the opening is lit; the ray
	// through the doorway's centre at height 100 lands 100 * (0.6 / 0.8)... on the floor z = 0.
	const float landing = 100.0f * (-toward.y / toward.z);  // y where the centre ray reaches z = 0
	CHECK(D::SunLit(opening, toward, { 0.0f, landing, 0.0f }) > 0.95f);
	// The doorway's top edge (z = 200) lands at y = 150 on the floor: further in is in shadow.
	CHECK(D::SunLit(opening, toward, { 0.0f, 165.0f, 0.0f }) < 0.01f);
	CHECK(D::SunLit(opening, toward, { 0.0f, 120.0f, 0.0f }) > 0.9f);
	// Sideways out of the doorway's width: dark. At the doorway's edge: half.
	CHECK(D::SunLit(opening, toward, { 90.0f, landing, 0.0f }) < 0.01f);
	CHECK(D::SunLit(opening, toward, { 0.0f, landing, 0.0f }) >= D::SunLit(opening, toward, { 47.0f, landing, 0.0f }));
	CHECK(D::SunLit(opening, toward, { 47.0f, landing, 0.0f }) > D::SunLit(opening, toward, { 55.0f, landing, 0.0f }));
	// The street's side of the plane gets no patch.
	CHECK(D::SunLit(opening, toward, { 0.0f, -50.0f, 0.0f }) == 0.0f);
	// A sun on the room's side (behind the room's far wall) shines in nothing.
	CHECK(D::SunLit(opening, { 0.0f, 0.6f, 0.8f }, { 0.0f, 100.0f, 0.0f }) == 0.0f);
	// A sun grazing along the doorway's plane lights nothing.
	CHECK(D::SunLit(opening, { 1.0f, 0.0f, 0.0f }, { 0.0f, 100.0f, 100.0f }) == 0.0f);
	// Far beyond the reach: gone.
	Vec3 low{ 0.0f, -0.99f, 0.14f };
	low.Unitize();
	CHECK(D::SunLit(opening, low, { 0.0f, 3000.0f, 0.0f }) == 0.0f);

	// The sky seen through the opening: more near it than far, none from behind, none for a
	// surface facing away.
	const Vec3 facingDoor{ 0.0f, -1.0f, 0.0f };
	const float near = D::SkyView(opening, { 0.0f, 100.0f, 100.0f }, facingDoor);
	const float far = D::SkyView(opening, { 0.0f, 600.0f, 100.0f }, facingDoor);
	CHECK(near > far && far > 0.0f && near < 1.0f);
	CHECK(D::SkyView(opening, { 0.0f, -100.0f, 100.0f }, facingDoor) == 0.0f);
	CHECK(D::SkyView(opening, { 0.0f, 100.0f, 100.0f }, { 0.0f, 1.0f, 0.0f }) == 0.0f);
	CHECK(D::SkyView(opening, { 0.0f, 100.0f, 100.0f }, {}) > 0.0f);

	// Gains: a shut door lets in nothing, night nothing, a clear day a lot, never over the cap.
	CHECK(D::GainsOf(1.4f, 0.3f, 0.7f, 0.0f).sun == 0.0f);
	CHECK(D::GainsOf(0.0f, 0.0f, -0.5f, 1.0f).sun == 0.0f);
	CHECK(D::GainsOf(1.4f, 0.3f, -0.1f, 1.0f).sun == 0.0f);  // the sun under the horizon
	const auto day = D::GainsOf(1.4f, 0.3f, 0.7f, 1.0f);
	CHECK(Near(day.sun, 1.4f) && Near(day.spill, 0.6f));
	CHECK(D::GainsOf(50.0f, 50.0f, 0.7f, 1.0f).sun <= D::kSunGainMost + 1e-4f);
	CHECK(D::GainsOf(50.0f, 50.0f, 0.7f, 1.0f).spill <= D::kSpillGainMost + 1e-4f);
	CHECK(D::GainsOf(1.4f, 0.3f, 0.7f, 0.3f).sun < day.sun);
}

void SunCorrectionLearns()
{
	using Core::Vec3;
	using Core::Lighting::SunCorrection;
	const auto dir = [](float a_azimuthDeg, float a_elevationDeg) {
		const float az = Core::DegToRad(a_azimuthDeg), el = Core::DegToRad(a_elevationDeg);
		return Vec3{ std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
	};

	SunCorrection correction;
	// Nothing learned: the formula as it is.
	CHECK(Near(correction.Apply(12.0f, dir(40.0f, -50.0f)), dir(40.0f, -50.0f)));
	// A mod puts the sun 13 degrees higher and 20 degrees round at 12:00.
	for (int i = 0; i < 20; ++i) {
		correction.Observe(12.1f, dir(40.0f, -50.0f), dir(60.0f, -37.0f));
	}
	CHECK(correction.Samples() == 1);
	CHECK(Near(correction.Apply(12.2f, dir(40.0f, -50.0f)), dir(60.0f, -37.0f), 2e-2f));
	// Held for the next hour and a half, gone five hours away.
	CHECK(Near(correction.Apply(13.4f, dir(40.0f, -50.0f)), dir(60.0f, -37.0f), 2e-2f));
	CHECK(Near(correction.Apply(18.0f, dir(40.0f, -50.0f)), dir(40.0f, -50.0f), 1e-3f));
	// Across midnight too (hour 23.9 and 0.1 are neighbours).
	SunCorrection night;
	night.Observe(23.9f, dir(10.0f, 20.0f), dir(15.0f, 20.0f));
	CHECK(Near(night.Apply(0.2f, dir(10.0f, 20.0f)), dir(15.0f, 20.0f), 2e-2f));
	// Between two visits the difference is interpolated.
	SunCorrection two;
	two.Observe(8.25f, dir(0.0f, 10.0f), dir(10.0f, 10.0f));
	two.Observe(10.25f, dir(0.0f, 10.0f), dir(30.0f, 10.0f));
	CHECK(Near(two.Apply(9.25f, dir(0.0f, 10.0f)), dir(20.0f, 10.0f), 3e-2f));
	// The azimuth difference wraps round the circle the short way.
	SunCorrection wrap;
	wrap.Observe(12.1f, dir(175.0f, 30.0f), dir(-175.0f, 30.0f));
	CHECK(Near(wrap.Apply(12.1f, dir(170.0f, 30.0f)), dir(180.0f, 30.0f), 2e-2f));
}

void StreetShadeMarch()
{
	namespace S = Core::StreetShade;
	using Core::Vec3;

	// What counts as inside something: not the surface itself, yes a little behind it, no far behind (a thin thing).
	CHECK(S::Hit(2.0f, 0.0f) == 0.0f && S::Hit(-30.0f, 0.0f) == 0.0f);
	CHECK(S::Hit(20.0f, 10.0f) > 0.99f);
	CHECK(S::Hit(300.0f, 10.0f) == 0.0f);
	// A far sample may be deeper behind its surface than a near one: big things cast the long shadows.
	CHECK(S::Hit(120.0f, 600.0f) > 0.9f && S::Hit(120.0f, 10.0f) == 0.0f);

	// A slab of something (x 100..260) the camera sees the front of: a ray through it is shaded, one beside it isn't.
	const auto slab = [](const Vec3& a_q) -> std::optional<float> { return a_q.x >= 100.0f && a_q.x <= 260.0f ? 20.0f : -50.0f; };
	const float through = S::Lit({ 0, 0, 0 }, { 1, 0, 0 }, S::kReach, 0.5f, S::kSteps, slab);
	CHECK(through < 0.8f && through > 0.0f);
	CHECK(S::Lit({ 0, 0, 0 }, { 0, 1, 0 }, S::kReach, 0.5f, S::kSteps, slab) == 1.0f);
	// Nothing seen along it (off screen, behind the camera): lit, as far as anyone can tell.
	CHECK(S::Lit({ 0, 0, 0 }, { 1, 0, 0 }, S::kReach, 0.5f, S::kSteps, [](const Vec3&) -> std::optional<float> { return std::nullopt; }) == 1.0f);
	// The samples crowd near the surface and stay within the reach.
	CHECK(S::SampleAlong(0, 0.0f, S::kSteps, S::kReach) == 0.0f && S::SampleAlong(S::kSteps - 1, 0.99f, S::kSteps, S::kReach) < S::kReach);
	CHECK(S::SampleAlong(3, 0.5f, S::kSteps, S::kReach) < 0.1f * S::kReach);

	// The sun's share of the light: all of it with no ambient, little under a dim sun, nothing facing away.
	CHECK(S::SunShare(1.5f, 0.0f, 1.0f) > 0.95f);
	CHECK(S::SunShare(0.2f, 0.5f, 1.0f) < 0.3f);
	CHECK(S::SunShare(1.5f, 0.3f, -0.4f) == 0.0f);
	// Strength: none at night or with the door shut, the most on a clear day; the multiplier never below 1 - most.
	CHECK(S::Strength(-0.3f, 1.0f) == 0.0f && S::Strength(0.8f, 0.0f) == 0.0f);
	CHECK(Near(S::Strength(0.8f, 1.0f), S::kMost));
	CHECK(S::Multiplier(S::kMost, 1.0f, 0.0f) >= 1.0f - S::kMost - 1e-4f && S::Multiplier(S::kMost, 1.0f, 1.0f) == 1.0f && S::Multiplier(S::kMost, 0.0f, 0.0f) == 1.0f);
}
