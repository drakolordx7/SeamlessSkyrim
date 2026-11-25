#pragma once

// Tests: which code made the GPU resources still alive (bridge `gpu_live`). Each texture and
// buffer the device creates from the first call on is kept with its size and the stack it was
// made from, and dropped when the resource is destroyed (ID3DDestructionNotifier). A resource
// that outlives what it was made for is listed under the code that made it.
namespace Bridge::GpuTrack
{
	// Main thread: starts tracking (once). Resources made before aren't listed.
	void Start();
	// Any thread: the live resources made since a_since (a mark, 0 = since the start), grouped by
	// the frames of their stack outside Direct3D and the driver, the largest first.
	json Live(std::uint64_t a_since, int a_groups, int a_frames);
	// The engine textures behind the largest live resources made since a_since: where in memory
	// a pointer to each is kept, and the class of the object holding it (MSVC RTTI).
	json Holders(std::uint64_t a_since, int a_samples);
	// The same for device resources whose engine texture is already destroyed.
	json OrphanHolders(std::uint64_t a_since, int a_samples);
	// Where in memory each of a_values is kept, and the class of the object keeping it.
	std::vector<json> Scan(const std::vector<std::uintptr_t>& a_values);
	// The mark the next resource made gets.
	std::uint64_t Mark();
}
