#pragma once

// What the portal modules report to the test tooling: the status of the portal and each module,
// the scene check run_suite uses as its oracle, and the measurements of the merged rooms. Read
// only; each module gives out its state through accessors.
namespace Portal
{
	class RemoteCell;
}

namespace Bridge::PortalDiagnostics
{
	[[nodiscard]] json Status();

	// Tests: still things (statics, trees, furniture...) within a_radius of the player in the player's
	// own space, and of the door in the merged space, that hang nowhere or aren't drawn where they
	// belong; the merged street's land and distant LOD the same. "ok": none found.
	[[nodiscard]] json SceneCheck(float a_radius);
	// Diagnostics: merged shapes (or their skin bones) farther than a_far from a_near.
	[[nodiscard]] json MergedShapesReport(const RE::NiPoint3& a_near, float a_far);
	// Diagnostics (portal_merge): the brightness of the view and of the rooms merged, and their scales.
	[[nodiscard]] json DescribeAdaptation();
	// A remote space: its cells, load state, lights, fade and people.
	[[nodiscard]] json DescribeSpace(const Portal::RemoteCell& a_space);
	// Diagnostics (portal_passes): the cached render passes of a space's shapes; a_drop forgets the
	// lists that hold passes already returned to the pool.
	[[nodiscard]] json RenderPasses(const Portal::RemoteCell& a_space, bool a_drop);
}
