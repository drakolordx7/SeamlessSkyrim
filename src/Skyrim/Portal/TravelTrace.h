#pragma once

// What a crossing did, frame by frame, for the bridge's portal_status (Travel::Status) and the
// log: the events of the crossing (with their time), the frames that took long, when the player
// arrived. Plain data, no engine state of its own; all of it on the main thread.
namespace Portal::TravelTrace
{
	// Once, at kDataLoaded. Agent sessions: [Portals] sProfileCrossingsTo, a folder in which every
	// crossing is sampled (Sampler) for 5 s into a numbered file. The long frames come up to a few
	// seconds after the crossing, beyond the bridge command's one and a half.
	void Install();

	// A crossing through a_door, from a_from, begins.
	void Begin(RE::FormID a_door, RE::TESObjectCELL* a_from);

	void Note(const std::string& a_what);

	// Per frame: records frame times while a crossing is being traced (3 s).
	void Update();

	// Diagnostics: sample the main thread's stacks for 1.5 s from the next crossing on (Sampler).
	void ProfileNext(const std::filesystem::path& a_file);

	[[nodiscard]] bool Running();
	// The last finished crossing's trace.
	[[nodiscard]] const json& Last();
}
