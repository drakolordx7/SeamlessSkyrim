# Render backends: Community Shaders fork, or the game's own shaders (and ENB)

Seamless doors need changes to how shapes are drawn: the doorway mask of render-merge
(two spaces drawn in one pass, each only where the other isn't), the far door's clip in the portal
pass, and the light, ambient and fog of the space behind the door. `src/Skyrim/Render/Backend.h` hides
who does that.

| Backend | When | What does the work |
|---|---|---|
| Community Shaders (fork) | `CommunityShaders.dll` with the `SeamlessPortal_*` exports | The fork's shaders (COMMUNITY-SHADERS-PATCH.md). Unchanged from before. |
| Native | No Community Shaders (vanilla shaders, and ENB, which runs on them) | `src/Skyrim/Render/Native/NativeMask.cpp`, `src/Skyrim/Render/Native/NativeLighting.cpp` |
| None | A stock Community Shaders (no fork exports) | Nothing: no render-merge; the portal pass is drawn unclipped, with only the sun and ambient the portal code sets. Crossing without a loading screen works. |

How many rooms can be merged at once (`Render::Backend::Interface::MergedRooms`): 4 with
the fork's per-room exports (`SeamlessPortal_SetMergedRoomDoor/Lighting`), else 1. `SetMergedDoor` and
`SetMergedLighting` take the room's slot; the native backend accepts slot 0 only.

Chosen once when the portal hooks are installed (`Plugin::Install`, kDataLoaded).
`[Render] sBackend` in `SeamlessSkyrim.ini` overrides: `auto` (default), `cs`, `native`, `none`.
`native` with Community Shaders loaded is allowed for testing: the mask works on CS's shaders too,
but CS lights the world itself, so the room's light doesn't match.

`dump_world` / `portal_status` show it under `render` (backend, why, ENB, and the native backend's
counters). The in-game menu shows the backend's name.

## Native: the doorway mask

No shader is changed. For a draw that must be cut, a geometry shader is bound behind the engine's
vertex shader. It passes every vertex on unchanged and adds up to six clip distances
(`SV_ClipDistance0/1`: X's five planes, and a city gate's sixth); the rasterizer cuts at them per
pixel. A plane's distance is linear in world space and clip distances interpolate
perspective-correctly, so the cut is exact.

- One room only: with two doorways, a shape near both would be cut into the pieces outside each
  doorway's X, multiplied; not solved. `[Portals] iShownRooms` above 1 keeps the other rooms loaded
  out of sight here.
- The rule is the fork's `SeamlessDoorClips`: X = beyond the door plane (5-unit margin) and seen
  through the doorway's rectangle. X is convex (the plane, plus four planes through the camera and
  the doorway's edges). The other space's shapes are kept in X (one draw); the camera's space's
  shapes outside X, drawn as up to five disjoint convex pieces. Through a city gate X ends where
  the portal pass's cells end (`SetGatePassage`): a sixth plane and piece. Shapes whose bounding sphere is
  50 units clear of a plane are drawn unchanged, or not at all, without a geometry shader. The rule
  and the pieces are plain-float code in `src/Core/Render/DoorwayPlanes.h` (`CutThroughDoorway`,
  `CutAt`); `NativeMask.cpp` turns the planes into clip space and draws.
- Planes are turned into clip space with the renderer's current view-projection (row vectors,
  camera-relative, as the doorway quad in `Pictures/Composite.cpp` uses it), so the portal pass's camera works the same way.
- Which draw: hooks on `SetupGeometry` / `RestoreGeometry` (vfuncs 6 and 7) of the Lighting,
  Utility and Effect shaders plan the cut for the pass between them. Main pass: depth target
  `kMAIN` and the world light scene, so shadow maps aren't cut. Portal pass: accumulator drawing
  with the portal light scene (State slot 3).
- The draw calls: `ID3D11DeviceContext` DrawIndexed / Draw / DrawIndexedInstanced /
  DrawInstanced in the context's function table. The context the engine draws with is the global
  `ID 411391`. Its table sits inside the context object and the runtime rewrites it (a hook put
  in once was gone after 17 draws), so the hooks are checked and put back before every draw that
  needs them, and each table's originals are kept.
- Geometry shaders are generated per vertex shader output signature: the engine keeps each vertex
  shader's bytecode (`BSGraphics::VertexShader::rawBytecode`), it is reflected, HLSL written,
  compiled with d3dcompiler (`gs_5_0`), and its signatures checked against the vertex shader's
  before use. About a dozen in Whiterun, ~50 ms in all, once per session.
- Effect shaders are cut too (the fork cuts only Lighting and Utility), so glow cards and
  particles in a merged room or before the far door are handled.

## Native: light, ambient and fog

`BSLightingShader::SetupGeometry` (ID 107300) reads, per draw: the pass's lights (sceneLights[0]
the sun, then point lights), how many to upload from bits 3-5 / 6-8 of the shader's raw technique
(+0x94; the vertex and pixel shaders don't depend on these bits, ID 108699 / 108700), and the
directional ambient at ID 390984. For the length of the engine's call these are swapped:

- Portal pass: the nearest lights of the portal light scene that reach the shape (up to 7,
  shadow-casting ones dimmed by `LightOcclusion`), as the fork's strict-light path does. The
  engine had paired almost none with the portal's shapes.
- Merged room: the room's lights (they stay in the portal light scene), and the sun colour and
  direction and ambient from `SetMergedLighting`, scaled by `RoomLightScale` (the room's
  brightness against the view's, measured; 1 within a few steps of the doorway).
- Street merged into an interior: the street's fog. Fog is uploaded with the technique (ID 107320
  from SetupTechnique), so the scene's fog property is given the street's values, the technique is
  set up again, and afterwards the property is put back and the batch renderer's cached technique
  (ID 415272) cleared. The SSAO composite (ID 107245 + 0xD26) fogs the picture by depth too and
  gets a copy of the fog property with the street's fog; without it the mountains seen through
  the doorway were black.
- Room merged into the street: its cell's own fog on its shapes. The SSAO composite fogs every
  pixel alike and keeps the street's (it takes the merged fog only with the player indoors).

Not done natively: Interior Sun (the street's sun shadows inside), skylighting settle, Sky Sync's
sun direction (the computed one is used). These are Community Shaders features. Nor, for a room
seen from across the street, the street's light through its doorway and the street's fog in front
of it (`SetMergedDaylight`, `SetFrontFog`): both are per pixel in the fork's shaders.

## ENB

`Backend.cpp` (`Enb::Detect`, in `src/Skyrim/Render/`) finds ENB's SDK exports (`ENBGetSDKVersion` in d3d11.dll / dxgi.dll) and
reports version and whether effects are on. ENB uses the native backend: no game shader is
replaced, so ENB's recognition of the game's shaders is untouched, and the mask leaves no depth
where it cuts, which ENB's screen-space effects read. With ENB the engine's context is ENB's proxy;
its function table is hooked the same way (untested: no ENB on the test machine).

What stays: ENB grades interiors and exteriors with separate settings and switches when the
current cell changes, i.e. at the moment of crossing. The view through a doorway is graded as the
space the player is in.

## Tested (AE 1.7.104, Whiterun: Belethor's, Bannered Mare)

- Vanilla shaders (CommunityShaders.dll moved aside): auto picks native; street through
  Belethor's doorway and the Bannered Mare's room from the street both masked cleanly; the portal
  pass (bMergeOutdoors=0) clipped and lit with the inn's lights; mountains through the doorway
  fogged instead of black. This load order is tuned for Community Shaders (inverse-square
  lights), so without it everything was overexposed, with or without portals.
- Community Shaders fork: auto picks it; looks as before.
- CS fork with sBackend=native: mask correct both ways, the room dark (CS ignores pass lights).
- Not tested: ENB; a stock Community Shaders; performance on a slow GPU (the cut costs a geometry
  shader on the shapes near the doorway and on every shape of the other space that crosses the
  doorway's edges).
