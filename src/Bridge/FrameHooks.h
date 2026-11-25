#pragma once

// Per-frame hooks the bridge needs: steering the player (walk_to) and grabbing the back buffer
// (screenshot). Both run inside the engine's own frame so they see consistent state.
namespace FrameHooks
{
	// Registers the bridge's parts of the frame: steering before the portals' update, and the
	// brightness log, screenshots and frame sequences at Present. At plugin load.
	void Register();

	struct WalkRequest
	{
		RE::ObjectRefHandle target;  // followed if valid, otherwise `point` is used
		RE::NiPoint3        point;
		float               stopDistance = 64.0f;
		bool                run = false;
		float               timeoutSeconds = 30.0f;
	};

	// Main thread only.
	void StartWalk(const WalkRequest& a_request);
	void StopWalk(std::string_view a_reason);
	json WalkStatus();

	// Diagnostics: frame times of the last 120 frames (average, median, 95th percentile, worst).
	json FrameStats();

	// Any thread except the main/render thread: blocks until the next Present wrote the file.
	json CaptureScreenshot(const std::wstring& a_path, std::chrono::milliseconds a_timeout);

	// Diagnostics: the next a_frames presented frames, each one, saved to a_directory as
	// frame_<index>_<ms since the request>.jpg once the last is taken. A flash lasting one frame
	// falls between screenshots, which are taken one at a time and slowly. False without the
	// Present hook.
	bool CaptureFrames(const std::wstring& a_directory, int a_frames);
	// While a directory is set (SetEventCapture, or SEAMLESS_TRAVEL_CAPTURE=<dir> at launch):
	// CaptureFrames into <dir>\<a_event>_<n>, for every crossing and merge. An event during
	// another's frames makes that run longer instead of starting one of its own.
	void CaptureEventFrames(std::wstring_view a_event, int a_frames);
	// Empty: off. Not left on for a whole session: writing a burst out is a hitch of its own,
	// which is what a stutter test is looking for.
	void SetEventCapture(const std::wstring& a_directory);

	// Diagnostics: average brightness (0..1) of every presented frame while recording, with its
	// wall-clock time (as in the log) and the time since the frame before. A flash lasting a frame
	// or two falls between screenshots; this sees each frame. a_start: clears and records anew.
	void RecordBrightness(bool a_start);
	json BrightnessLog();
}
