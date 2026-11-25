#pragma once

// Pictures of doorways, for a door that swings open before the space behind it has loaded
// (Manager::StandIn). Whenever a doorway stands fully open on the real space, what is seen through
// it is copied out of the frame, small, and kept per door (on disk, in SeamlessSkyrim_Doorways);
// for a door to the outdoors one per part of the day. Shown blurred, so that what changed since
// (people, light, the angle it is seen from) doesn't show, and faded out once the real space is
// there. A door never seen open gets a plain colour instead.
//
// Each texel also keeps how far behind the doorway its surface was, and the picture where it was
// taken from: drawn flat on the doorway it read as a board standing in it, nothing behind it
// moving as the player did. It is drawn as the room in relief instead, the eye's ray marched
// through it.
namespace Portal::Snapshot
{
	// The main view as the doorway is drawn with (Render's MainView).
	struct View
	{
		float        viewProj[16];                    // camera-relative, row vectors, as drawn (jittered)
		float        viewProjInverse[16];             // of viewProj
		float        viewProjUnjittered[16];
		float        previousViewProjUnjittered[16];  // relative to the previous frame's eye
		RE::NiPoint3 eye;                             // the world position the matrices are relative to
		RE::NiPoint3 eyeDelta;                        // this frame's eye - the previous frame's
		float        width = 0;                       // the scene's viewport in pixels (render scale applied)
		float        height = 0;
		// The fog of the player's space, as the engine's shaders take it: param (near / (far - near),
		// 1 / (far - near), power, most it covers), near colour, far colour. The world is fogged by
		// depth in the SSAO composite, before doorway pictures are drawn, so they fog themselves.
		std::array<float, 12> fog{};
	};

	// The doorway's corners: bottom, bottom (across), top, top (across), as Doors::Quad gives them.
	using Corners = std::array<RE::NiPoint3, 4>;

	// What the doorway's real space was drawn into this frame (ID3D11ShaderResourceViews): the main
	// scene and its depth when the space is merged into the world, or the portal pass's own.
	struct Source
	{
		void* color = nullptr;
		void* depth = nullptr;
		// The portal pass's (portalFar > 0): its image fills its target at any render scale, and
		// its near plane is at the door, so its depth values mean other distances than the main
		// view's.
		float portalNear = 0.0f;
		float portalFar = 0.0f;
		// The portal pass drew this frame with the capture camera (TakeCaptureRequest), not the
		// player's view.
		bool captureCamera = false;
		// How bright the room's own lights were drawn in it (Manager::RoomLightScale): turned down
		// for a room merged into the street, not in the portal pass.
		float lightScale = 1.0f;
	};

	// A camera of its own for the doorway's picture, in front of the doorway's middle and looking
	// straight through it, wide enough to see the room beside and above it too. A picture taken
	// with the player's view had only what the doorway let through from where they stood, so a
	// step aside showed the doorway's edge smeared over what the wall had hidden; and from a
	// fixed place the picture is the same whoever takes it. The portal pass draws one frame with
	// it into the targets not on screen (the doorway keeps the last picture for that frame).
	struct CaptureCamera
	{
		RE::FormID      door = 0;
		RE::NiTransform transform;  // in the player's space, as an NiCamera's (looking along its rotation's first column)
		float           left = 0.0f, right = 0.0f, bottom = 0.0f, top = 0.0f;  // the frustum at distance 1
		float           toDoorway = 0.0f;  // the door plane's distance ahead, in the player's space
	};

	// Render (main thread), after the world is drawn into the main target and before first person:
	// takes the doorway's picture if none was taken lately. From the portal pass it is taken with
	// the capture camera, which Capture asks for (TakeCaptureRequest) and waits a frame; merged into
	// the world, with the player's view if that sees the doorway well enough, unless there is a
	// capture camera's picture already. A room is merged as soon as it has loaded, so the manager
	// holds the merge back for a door without one (Manager::PictureWanted).
	// a_gate: a city gate's doorway, taken from further back (its far side is a street, not a room).
	void Capture(const View& a_view, const Source& a_source, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners);

	// Render, before the portal pass: the capture camera, once after Capture asked for it.
	std::optional<CaptureCamera> TakeCaptureRequest();

	// The capture camera for a room door's doorway (a_corners as Capture's; a_eye the side it is seen from).
	CaptureCamera CameraFor(RE::FormID a_door, const Corners& a_corners, const RE::NiPoint3& a_eye);

	// Once, at kDataLoaded: the shaders compiled behind the startup screen, not on the first
	// doorway of a session.
	void Prewarm();

	// Main thread: the door has a picture (for some part of the day, for a door to the outdoors).
	bool HasPicture(RE::FormID a_door, bool a_outdoors);

	// Main thread: the door into a room has no picture taken with the capture camera at the
	// current size. Such a picture is kept once taken: from its fixed place it comes out the same.
	// a_corners: the doorway as the capture camera takes it (its shape sets the picture's).
	bool CameraPictureWanted(RE::FormID a_door, const Corners& a_corners);

	// How the space behind the doorway is lit as seen from the player's. A picture is drawn as
	// bright as the real space would be (lightScale against the one it was taken under). The
	// plain colour has no light of its own to go by: by day a room seen from the street is made
	// dark, the street seen from a room bright. A picture once had that too, and came out brighter
	// or darker than the real space fading in over it.
	struct Adaptation
	{
		enum class Kind
		{
			kNone,              // room to room
			kRoomFromOutdoors,
			kOutdoorsFromRoom,
		};
		Kind  kind = Kind::kNone;
		float daylight = 0.0f;    // 0 at night .. 1 in full day
		float lightScale = 1.0f;  // the room's own lights, as Source::lightScale, right now
	};

	// Render, as Capture: draws the door's picture (or a_plain, without one) into the doorway,
	// with a_alpha. Through a gate (a_gate) the picture's sky is left to the player's own.
	void Draw(const View& a_view, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners, float a_alpha, const RE::NiColor& a_plain, const Adaptation& a_adaptation);

	// Render, every frame: pictures taken a few frames ago written to disk. Left to Capture and
	// Draw, a picture taken just before turning to third person or walking through waited for the
	// next doorway seen, and was never written if the game was quit first.
	void Flush();

	// The size pictures are taken at, as their height in texels; the width is three quarters of it.
	// Pictures already taken keep their size until taken again (at once, when next seen).
	int  PictureHeight();
	void SetPictureHeight(int a_height);

	// Any thread: every picture deleted from disk (returns how many) and, at the main thread's next
	// use, from memory. Each door is taken again when next seen open.
	int DeleteAll();

	// Brightness as an eye judges a scene: the geometric mean of the luminance, in the main scene's
	// units (before tone mapping). Skyrim has no lasting eye adaptation (the tone map's exposure is
	// the ratio of two adapted copies of the same average, 1 once settled), so a room seen from a
	// sunlit street came out as bright as from inside it; these are what Manager::RoomLightScale
	// compares instead.
	//
	// Render, after the world is drawn: measures a_scene (the main scene's view), read back a few
	// frames later. a_width, a_height: its viewport in pixels.
	void MeasureScene(void* a_scene, float a_width, float a_height);
	// The player's view, as an eye takes a second or so to adapt to it; none before the first reading.
	std::optional<float> SceneBrightness();
	// The room behind a door as drawn with its own light (its picture, divided by the light scale it
	// was taken with). Without one, the rooms' with pictures; none before any.
	std::optional<float> RoomBrightness(RE::FormID a_door);

	json Describe();
}
