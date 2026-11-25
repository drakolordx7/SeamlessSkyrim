#include "Core/Pictures/Layers.h"

#include <algorithm>
#include <cmath>

namespace Core::Pictures::Layers
{
	std::array<Place, kMaxLayers> PlacesFor(int a_count)
	{
		switch (std::clamp(a_count, 1, kMaxLayers)) {
		case 1:
			return { Place{ 0.5f, kEyeUp, kInsideDepth }, {}, {} };
		case 2:
			return { Place{ 0.3f, kEyeUp, kInsideDepth }, Place{ 0.7f, kEyeUp, kInsideDepth }, {} };
		default:
			// The middle first: with it alone the room reads, the sides add what it can't see.
			return { Place{ 0.5f, kEyeUp, kInsideDepth }, Place{ 0.12f, kEyeUp, kInsideDepth }, Place{ 0.88f, kEyeUp, kInsideDepth } };
		}
	}

	View ViewAt(const Place& a_place, float a_width, float a_height)
	{
		return { { a_place[0] * a_width, a_place[1] * a_height, a_place[2] }, kNearZ };
	}

	std::pair<std::uint32_t, std::uint32_t> SizeOf(int a_height)
	{
		const float aspect = (kS1 - kS0) / (kT1 - kT0);
		const auto  width = static_cast<std::uint32_t>(std::lround(a_height * aspect / 4.0f)) * 4u;
		return { width, static_cast<std::uint32_t>(a_height) };
	}

	Axes::Axes(const Camera::Frame& a_frame) :
		origin(a_frame.origin), across(a_frame.across), up(a_frame.up), inward(a_frame.inward), width(a_frame.across.Length()), height(a_frame.up.Length())
	{
		across = across / width;
		up = up / height;
	}

	Vec3 Axes::Local(const Vec3& a_world) const
	{
		const auto offset = a_world - origin;
		return { offset.Dot(across), offset.Dot(up), offset.Dot(inward) };
	}

	Vec3 Axes::World(const Vec3& a_local) const
	{
		return origin + across * a_local.x + up * a_local.y + inward * a_local.z;
	}

	Camera::CaptureCamera CameraFor(const Camera::Frame& a_frame, const View& a_view)
	{
		const Axes            axes(a_frame);
		Camera::CaptureCamera camera;
		const auto            forward = axes.inward;
		const auto            right = forward.Cross(axes.up);  // an NiCamera's columns: forward, up, right
		for (int row = 0; row < 3; ++row) {
			camera.transform.rotate.entry[row][0] = forward[row];
			camera.transform.rotate.entry[row][1] = axes.up[row];
			camera.transform.rotate.entry[row][2] = right[row];
		}
		camera.transform.translate = axes.World(a_view.position);
		camera.transform.scale = 1.0f;
		const float acrossOnScreen = right.Dot(axes.across);
		const float side0 = acrossOnScreen * kS0;
		const float side1 = acrossOnScreen * kS1;
		camera.left = std::min(side0, side1);
		camera.right = std::max(side0, side1);
		camera.bottom = kT0;
		camera.top = kT1;
		camera.toDoorway = -a_view.position.z;
		camera.nearClip = a_view.nearZ;
		return camera;
	}

	std::array<std::array<float, 4>, 3> CaptureCorners(const Camera::CaptureCamera& a_camera, const Camera::Frame& a_frame, const View& a_view)
	{
		const Axes     axes(a_frame);
		constexpr float kAhead = 100.0f;
		const auto     at = [&](float a_s, float a_t) {
            return Camera::ClipOfPoint(a_camera, axes.World(a_view.position + Vec3{ a_s * kAhead, a_t * kAhead, kAhead }));
		};
		return { at(kS0, kT0), at(kS1, kT0), at(kS0, kT1) };
	}

	Projection Project(const View& a_view, const Vec3& a_local)
	{
		Projection out;
		const auto relative = a_local - a_view.position;
		out.z = relative.z;
		if (relative.z <= a_view.nearZ) {
			return out;
		}
		out.s = relative.x / relative.z;
		out.t = relative.y / relative.z;
		out.u = (out.s - kS0) / (kS1 - kS0);
		out.v = 1.0f - (out.t - kT0) / (kT1 - kT0);
		out.inView = out.s >= kS0 && out.s <= kS1 && out.t >= kT0 && out.t <= kT1;
		return out;
	}

	Vote VoteOf(const View& a_view, const Projection& a_projection, float a_stored, bool a_exact)
	{
		if (!a_projection.inView) {
			return Vote::kNone;
		}
		if (a_stored >= kNoSurface) {
			return Vote::kFree;
		}
		const float residual = a_projection.z - (a_stored - a_view.position.z);
		const float tolerance = a_exact ? 0.0f : kToleranceAbs + kToleranceRel * a_projection.z;
		return residual < -tolerance ? Vote::kFree : residual > tolerance ? Vote::kBehind : Vote::kOn;
	}

	Setup SetupFor(int a_count, float a_width, float a_height, int a_rows)
	{
		Setup setup;
		setup.count = std::clamp(a_count, 1, kMaxLayers);
		const auto places = PlacesFor(setup.count);
		for (int i = 0; i < setup.count; ++i) {
			setup.views[i] = ViewAt(places[i], a_width, a_height);
		}
		setup.width = a_width;
		setup.height = a_height;
		const auto [across, down] = SizeOf(a_rows);
		setup.texelsAcross = static_cast<float>(across);
		setup.texelsDown = static_cast<float>(down);
		return setup;
	}

	namespace
	{
		struct Sample
		{
			Votes votes;
			int   layer = -1;
			float u = 0.0f, v = 0.0f;
			bool  covered = false;
		};

		Sample Look(const Setup& a_setup, const DepthAt& a_depth, const Vec3& a_point, bool a_exact = false)
		{
			Sample sample;
			float  best = 1e30f;
			for (int i = 0; i < a_setup.count; ++i) {
				const auto projection = Project(a_setup.views[i], a_point);
				if (!projection.inView) {
					continue;
				}
				float u = projection.u, v = projection.v;
				if (a_setup.texelsAcross > 0.0f) {
					// Nearest texel, as the shader's Load.
					u = (std::min(std::floor(u * a_setup.texelsAcross), a_setup.texelsAcross - 1.0f) + 0.5f) / a_setup.texelsAcross;
					v = (std::min(std::floor(v * a_setup.texelsDown), a_setup.texelsDown - 1.0f) + 0.5f) / a_setup.texelsDown;
				}
				const float stored = a_depth(i, u, v);
				const auto  vote = VoteOf(a_setup.views[i], projection, stored, a_exact);
				sample.covered = true;
				sample.votes.Add(vote);
				if (vote == Vote::kOn || vote == Vote::kBehind) {
					const float residual = std::abs(projection.z - (stored - a_setup.views[i].position.z));
					if (residual < best) {
						best = residual;
						sample.layer = i;
						sample.u = projection.u;
						sample.v = projection.v;
					}
				}
			}
			return sample;
		}

		Vec3 AtPath(const Vec3& a_origin, const Vec3& a_direction, float a_p)
		{
			return a_origin + a_direction * (DepthOf(a_p) / a_direction.z);
		}
	}

	Hit March(const Setup& a_setup, const DepthAt& a_depth, const Vec3& a_origin, const Vec3& a_direction)
	{
		Hit   hit;
		bool  wasCovered = false;
		float before = 0.0f;
		for (int step = 1; step <= kSteps; ++step) {
			const float p = kFarFraction * static_cast<float>(step) / kSteps;
			const auto  point = AtPath(a_origin, a_direction, p);
			const auto  sample = Look(a_setup, a_depth, point);
			if (!sample.covered) {
				if (wasCovered) {
					return hit;  // out the side of every picture
				}
				// Before the views begin the ray is in the doorway's depth: out of its opening there is
				// the wall, which no layer sees.
				if (point.x < 0.0f || point.x > a_setup.width || point.y < 0.0f || point.y > a_setup.height) {
					return hit;
				}
				before = p;
				continue;
			}
			wasCovered = true;
			if (!sample.votes.Solid()) {
				before = p;
				continue;
			}
			float low = before, high = p;
			for (int i = 0; i < kRefine; ++i) {
				const float middle = 0.5f * (low + high);
				if (Look(a_setup, a_depth, AtPath(a_origin, a_direction, middle), true).votes.Solid()) {
					high = middle;
				} else {
					low = middle;
				}
			}
			const auto found = Look(a_setup, a_depth, AtPath(a_origin, a_direction, high));
			hit.hit = found.layer >= 0;
			hit.point = AtPath(a_origin, a_direction, high);
			hit.layer = found.layer;
			hit.u = found.u;
			hit.v = found.v;
			return hit;
		}
		return hit;
	}
}
