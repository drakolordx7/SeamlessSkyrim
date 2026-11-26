// Included inside CoreTests.cpp's anonymous namespace (it uses its CHECK and Near); the standard and
// Core headers it needs are included there first.

void LayeredDepthImages()
{
	namespace L = Core::Pictures::Layers;
	using Core::Vec3;

	// A doorway 100 wide and 200 high. A room behind it: a back wall at z = 400, and a pole
	// 10 wide whose front face stands at z = 200 in the middle of the doorway.
	constexpr float kWidth = 100.0f, kHeight = 200.0f;
	const auto      setupFor = [&](int a_count) { return L::SetupFor(a_count, kWidth, kHeight, 512); };
	const auto      depth = [&](const L::Setup& a_setup) {
        return [&a_setup](int a_layer, float a_u, float) {
            const auto& view = a_setup.views[a_layer];
            const float s = L::kS0 + a_u * (L::kS1 - L::kS0);
            const float x = view.position.x + s * (200.0f - view.position.z);
            return x >= 45.0f && x <= 55.0f ? 200.0f : 400.0f;
        };
	};

	// The ray from the eye (50, 110, -300) through the doorway at (30, 110): meets the back wall.
	for (const int count : { 1, 2, 3 }) {
		const auto  setup = setupFor(count);
		const auto  fn = depth(setup);
		Vec3        direction = Vec3{ -20.0f, 0.0f, 300.0f };
		direction.Unitize();
		const auto hit = L::March(setup, fn, { 30.0f, 110.0f, 0.0f }, direction);
		CHECK(hit.hit);
		CHECK(Near(hit.point.z, 400.0f, 3.0f));
		CHECK(Near(hit.point.x, 30.0f - 20.0f / 300.0f * 400.0f, 1.0f));
	}

	// A ray that passes beside the pole (at z = 200 it is 3 units clear of its edge) and goes behind
	// it in the middle view's image. One view alone stops at the pole's stretched edge; with the
	// two at the sides, which see past it, the ray goes on to the wall.
	{
		Vec3 direction{ -0.1f, 0.0f, 1.0f };
		direction.Unitize();
		const Vec3 origin{ 78.0f, 110.0f, 0.0f };
		{
			const auto setup = setupFor(1);
			const auto hit = L::March(setup, depth(setup), origin, direction);
			CHECK(hit.hit && hit.point.z < 300.0f);
		}
		{
			const auto setup = setupFor(3);
			const auto hit = L::March(setup, depth(setup), origin, direction);
			CHECK(hit.hit && Near(hit.point.z, 400.0f, 3.0f) && Near(hit.point.x, 38.0f, 1.0f));
		}
	}

	// Very oblique, it is out of the doorway's opening before any view has begun: the image tier's.
	{
		const auto setup = setupFor(3);
		Vec3       direction{ 0.985f, 0.0f, 0.17f };
		direction.Unitize();
		CHECK(!L::March(setup, depth(setup), { 50.0f, 110.0f, 0.0f }, direction).hit);
	}

	// What the votes say.
	{
		L::Votes votes;
		CHECK(!votes.Solid());
		votes.Add(L::Vote::kBehind);
		CHECK(votes.Solid());
		votes.Add(L::Vote::kFree);
		CHECK(!votes.Solid());
		votes.Add(L::Vote::kOn);
		CHECK(votes.Solid());  // a surface seen as often as it is seen through
		votes.Add(L::Vote::kFree);
		CHECK(!votes.Solid());
	}
	{
		const L::View view{ { 50.0f, 110.0f, 36.0f }, L::kNearZ };
		const auto    at = [&](float a_z) { return L::Project(view, { 50.0f, 110.0f, a_z }); };
		CHECK(L::VoteOf(view, at(236.0f), 400.0f) == L::Vote::kFree);
		CHECK(L::VoteOf(view, at(400.0f), 400.0f, true) != L::Vote::kFree && L::VoteOf(view, at(399.0f), 400.0f, true) == L::Vote::kFree);
		CHECK(L::VoteOf(view, at(400.0f), 400.0f) == L::Vote::kOn);
		CHECK(L::VoteOf(view, at(500.0f), 400.0f) == L::Vote::kBehind);
		CHECK(L::VoteOf(view, at(20.0f), 400.0f) == L::Vote::kNone);  // behind the view
		CHECK(L::VoteOf(view, L::Project(view, { 50.0f, 110.0f, 300.0f }), 20000.0f) == L::Vote::kFree);  // the sky
		CHECK(L::VoteOf(view, L::Project(view, { 5000.0f, 110.0f, 100.0f }), 400.0f) == L::Vote::kNone);  // out of the extent
	}

	// The plan and the layers' sizes.
	{
		const auto one = L::PlacesFor(1), three = L::PlacesFor(3);
		CHECK(one[0][0] == 0.5f && three[0][0] == 0.5f && three[1][0] < 0.5f && three[2][0] > 0.5f && three[1][2] == L::kInsideDepth);
		const auto [width, height] = L::SizeOf(512);
		CHECK(height == 512 && width % 4 == 0 && Near(static_cast<float>(width) / height, 3.4f / 2.6f, 0.01f));
	}

	// The layer's camera: its corners in clip space are the corners of its image, and the middle of
	// the extent is the middle of it.
	{
		namespace C = Core::Pictures::Camera;
		const Core::Quad corners{ Vec3{ 0, 0, 0 }, Vec3{ 100, 0, 0 }, Vec3{ 0, 0, 200 }, Vec3{ 100, 0, 200 } };
		const C::Frame   frame(corners, { 50, -300, 100 });
		const auto       setup = setupFor(3);
		for (int i = 0; i < 3; ++i) {
			const auto camera = L::CameraFor(frame, setup.views[i]);
			const auto clip = L::CaptureCorners(camera, frame, setup.views[i]);
			CHECK(Near(std::abs(clip[0][0]), 1.0f) && Near(clip[0][1], -1.0f) && Near(clip[1][0], -clip[0][0]) && Near(clip[1][1], -1.0f));
			CHECK(Near(clip[2][0], clip[0][0]) && Near(clip[2][1], 1.0f));
			CHECK(Near(camera.toDoorway, -L::kInsideDepth) && Near(camera.nearClip, L::kNearZ));
			const auto middle = C::ClipOfPoint(camera, L::Axes(frame).World(setup.views[i].position + Vec3{ 0, 0, 100 }));
			CHECK(Near(middle[0], 0.0f) && Near(middle[1], 0.0f));
		}
	}
}

void ExposureBudgetAndFiles()
{
	namespace P = Core::Pictures;

	// A dim room from a bright street is dimmer, by the square root; never black, and (as asked)
	// never brighter than it is from inside.
	CHECK(Near(P::Exposure::ViewScale(0.1f, 4.0f), std::sqrt(0.025f), 1e-4f));
	CHECK(P::Exposure::ViewScale(4.0f, 0.1f) == 1.0f);
	CHECK(Near(P::Exposure::ViewScale(4.0f, 1.0f, 2.0f), 2.0f));
	CHECK(P::Exposure::ViewScale(1e-6f, 1.0f) == P::Exposure::kDarkest);
	CHECK(P::Exposure::ViewScale(0.0f, 1.0f) == 1.0f && P::Exposure::ViewScale(1.0f, 0.0f) == 1.0f);
	CHECK(P::Exposure::ViewScale(2.0f, 2.0f) == 1.0f);

	// The oldest go first, and only as many as it takes.
	{
		const std::vector<P::Budget::File> files{ { 10, 5 }, { 10, 1 }, { 10, 3 } };
		const auto                         out = P::Budget::Evict(files, 15);
		CHECK(out.size() == 2 && out[0] == 1 && out[1] == 2);
		CHECK(P::Budget::Evict(files, 30).empty());
		CHECK(P::Budget::Evict(files, 0).size() == 3);
	}

	// The load order: names in order, case aside; another order is another load order.
	{
		const auto a = P::LoadOrderHash({ "Skyrim.esm", "Update.esm", "Mod.esp" });
		CHECK(a == P::LoadOrderHash({ "skyrim.esm", "UPDATE.ESM", "mod.esp" }));
		CHECK(a != P::LoadOrderHash({ "Skyrim.esm", "Mod.esp", "Update.esm" }));
		CHECK(a != P::LoadOrderHash({ "Skyrim.esm", "Update.esm" }));
		CHECK(P::LoadOrderHash({ "ab", "c" }) != P::LoadOrderHash({ "a", "bc" }));
		const auto name = P::FileName("Skyrim.esm|01A6FB#day", 0x1234abcdull, false);
		CHECK(name == "Skyrim.esm_01A6FB_day@1234abcd.ssdw");
		CHECK(P::FileName("Skyrim.esm|01A6FB#inside", 0x1234abcdull, true).ends_with(".ssdl"));
	}

	// Headers: this version and this load order only.
	{
		P::Files::ImageHeader image;
		image.width = 640;
		image.height = 512;
		image.loadOrder = 7;
		CHECK(P::Files::Valid(image, 7, 1024, 1024));
		CHECK(!P::Files::Valid(image, 8, 1024, 1024));
		image.version = P::kVersion - 1;
		CHECK(!P::Files::Valid(image, 7, 1024, 1024));
		P::Files::LayeredHeader layered;
		layered.width = 668;
		layered.height = 512;
		layered.layers = 3;
		layered.loadOrder = 7;
		CHECK(P::Files::Valid(layered, 7, 1024, 1024) && P::Files::TexelBytes(layered) == P::Compress::PictureBytes(668, 512) * 3);
		layered.layers = 4;
		CHECK(!P::Files::Valid(layered, 7, 1024, 1024));
		CHECK(P::kVersion == 8);
	}

	// The image tier's size: the doorway at the size set, no extra density.
	{
		namespace C = P::Camera;
		const auto [width, height] = C::SizeFor(C::kRoomCamera.extent, 512, C::kNarrowest);
		CHECK(height == 896 && width == 1344);
	}
}

// Block compression: the sizes it takes, and the colour compressor against the format's own decode.
void CompressedPictures()
{
	namespace P = Core::Pictures;
	namespace Z = Core::Pictures::Compress;
	namespace B = Core::Pictures::Bc6h;

	// BC6H is a byte a texel; the chain adds a third; the distances are two bytes a texel.
	CHECK(Z::ColourLevelBytes(1344, 896, 0) == 336ull * 224 * 16);
	CHECK(Z::MipCount(1344, 896) == 11 && Z::MipCount(668, 512) == 10 && Z::MipCount(1, 1) == 1);
	CHECK(Z::ColourLevelBytes(1344, 896, 10) == 16);  // 1 x 1 is a block
	CHECK(Z::ColourBytes(1344, 896) > 1344ull * 896 && Z::ColourBytes(1344, 896) < 1344ull * 896 * 4 / 3 + 4096);
	CHECK(Z::DepthBytes(1344, 896) == 1344ull * 896 * 2);
	// against the half-float RGBA with mips it replaced: under a third
	CHECK(Z::PictureBytes(1344, 896) * 3 < 1344ull * 896 * 8 * 4 / 3);
	CHECK(Z::PictureBytes(668, 512) * 3 < 668ull * 512 * 8 * 4 / 3);
	// sizes are whole blocks
	{
		namespace C = P::Camera;
		for (const int height : { 32, 100, 128, 516, 1024 }) {
			const auto [w, h] = C::SizeFor(C::kRoomCamera.extent, height, C::kNarrowest);
			const auto [lw, lh] = P::Layers::SizeOf(height);
			const auto [dw, dh] = C::SizeFor(C::kDoorwayOnly, height, 1.3125f);
			CHECK(w % 4 == 0 && h % 4 == 0 && lw % 4 == 0 && lh % 4 == 0 && dw % 4 == 0 && dh % 4 == 0);
		}
	}

	// Half floats.
	CHECK(B::HalfBits(1.0f) == 0x3C00 && B::HalfBits(0.5f) == 0x3800 && B::HalfBits(-3.0f) == 0 && B::HalfBits(1e9f) == 0x7BFF);
	CHECK(B::HalfValue(0x3C00) == 1.0f && Near(B::HalfValue(B::HalfBits(0.1234f)), 0.1234f, 1e-4f));

	// A block: a ramp of brightness and hue, a lamp among dim wall, and a flat colour. The decode is the
	// format's (mode 11); it stays within a few percent of the half float, and no more.
	const auto relative = [](const B::Texels& a_in, const B::Texels& a_out) {
		float worst = 0.0f;
		for (std::size_t t = 0; t < 16; ++t) {
			for (std::size_t c = 0; c < 3; ++c) {
				worst = std::max(worst, std::abs(a_out[t][c] - a_in[t][c]) / std::max(a_in[t][c], 0.02f));
			}
		}
		return worst;
	};
	{
		B::Texels ramp{};
		for (std::size_t t = 0; t < 16; ++t) {
			const float s = static_cast<float>(t) / 15.0f;
			ramp[t] = { 0.2f + 3.0f * s, 0.15f + 1.8f * s, 0.1f + 0.7f * s };
		}
		const auto block = B::Encode(ramp);
		CHECK((block.bytes[0] & 31) == 3);
		// a straight ramp in linear light is a curve in the half float's bits, which the 16 steps follow within a few percent
		CHECK(relative(ramp, B::Decode(block)) < 0.15f);
	}
	{
		B::Texels lamp{};
		for (std::size_t t = 0; t < 16; ++t) {
			lamp[t] = { 0.30f, 0.22f, 0.15f };
		}
		lamp[5] = { 40.0f, 30.0f, 12.0f };
		const auto out = B::Decode(B::Encode(lamp));
		CHECK(Near(out[5][0] / 40.0f, 1.0f, 0.06f));
		CHECK(Near(out[0][0], 0.30f, 0.30f * 0.35f));  // the format has 16 steps between the two: the wall is somewhere near
	}
	{
		B::Texels flat{};
		for (auto& t : flat) {
			t = { 0.5f, 0.25f, 0.125f };
		}
		CHECK(relative(flat, B::Decode(B::Encode(flat))) < 0.04f);
		B::Texels black{};
		CHECK(B::Decode(B::Encode(black))[7][1] < 1e-3f);
	}
}
