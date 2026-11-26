# Rendering: the game's own shaders (and ENB)

Seamless doors need changes to how shapes are drawn: the doorway mask of render-merge
(two spaces drawn in one pass, each only where the other isn't), the far door's clip in the portal
pass, and the light, ambient and fog of the space behind the door. One renderer does it, on the
game's own shaders (which ENB runs on too), in `src/Skyrim/Render/Native/`:
`NativeMask.cpp` (the mask), `NativeLighting.cpp` (light, ambient, fog), `Pass.cpp` (which pass
the engine is drawing). It is hooked once when the portal hooks are installed
(`Plugin::Install`, kDataLoaded). If it can't hook the renderer (no device context) rooms aren't
drawn into the world (`Render::Native::Ready()`); crossing without a loading screen still works,
and the portal pass is drawn unclipped.

One room is merged at a time (the shown one): with two doorways, a shape near both would be cut
into the pieces outside each doorway's X, multiplied; not solved. `[Portals] iLoadedRooms` above 0
keeps the other rooms loaded out of sight; their open doors show a picture.

With Community Shaders installed the mask still works on its pixel shaders, but Community Shaders
lights the world itself, so the room's light doesn't match; not supported.

`dump_world` / `portal_status` show it under `render` (ready, ENB, and the mask's and the
lighting's counters).

## Native: the doorway mask

No shader is changed. For a draw that must be cut, a geometry shader is bound behind the engine's
vertex shader. It passes every vertex on unchanged and adds up to six clip distances
(`SV_ClipDistance0/1`: X's five planes, and a city gate's sixth); the rasterizer cuts at them per
pixel. A plane's distance is linear in world space and clip distances interpolate
perspective-correctly, so the cut is exact.

- The rule: X = beyond the door plane (5-unit margin) and seen through the doorway's rectangle. X is convex (the plane, plus four planes through the camera and
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
- Effect shaders are cut too, so glow cards and particles in a merged room or before the far door
  are handled.

## Native: light, ambient and fog

`BSLightingShader::SetupGeometry` (ID 107300) reads, per draw: the pass's lights (sceneLights[0]
the sun, then point lights), how many to upload from bits 3-5 / 6-8 of the shader's raw technique
(+0x94; the vertex and pixel shaders don't depend on these bits, ID 108699 / 108700), and the
directional ambient at ID 390984. For the length of the engine's call these are swapped:

- Portal pass: the nearest lights of the portal light scene that reach the shape (up to 7,
  shadow-casting ones dimmed by `LightOcclusion`). The engine had paired almost none with the
  portal's shapes.
- Merged room: the room's lights (they stay in the portal light scene), and the sun colour and
  direction and ambient from `Native::SetMergedLighting`, scaled by `RoomLightScale` (the room's
  brightness against the view's, measured; 1 within a few steps of the doorway).
- Street merged into an interior: the street's fog. Fog is uploaded with the technique (ID 107320
  from SetupTechnique), so the scene's fog property is given the street's values, the technique is
  set up again, and afterwards the property is put back and the batch renderer's cached technique
  (ID 415272) cleared. The SSAO composite (ID 107245 + 0xD26) fogs the picture by depth too and
  gets a copy of the fog property with the street's fog; without it the mountains seen through
  the doorway were black.
- Room merged into the street: its cell's own fog on its shapes. The SSAO composite fogs every
  pixel alike and keeps the street's (it takes the merged fog only with the player indoors).

Not done: the street's sun shadows inside an interior, and the street's fog in front of a room
seen from across the street (KNOWN-ISSUES.md, "Look features lost with the removed fork").

## Rain and snow at the doorway

The weather's rain and snow are one particle shape (ENGINE.md, "Rain and snow") that follows the
camera. The mask treats it as any shape, in the main pass, by whose space it belongs to:

- Outdoors, a room merged into the street: the shape is the street's (the player's space), so it is
  cut inside X: no rain or snow falls in the room seen through the doorway.
- Indoors, the street merged into the interior: `MergedView::IsMergedShape` names the shape the
  street's while the street is merged, so it is kept only inside X (beyond the doorway). The engine
  only gives it particles when the sky is in full mode; `PrecipitationUpdateHook` (`Render/Frame.cpp`,
  ID 26179) runs the update in full mode while the street is merged, so the street through the
  doorway has the weather's rain and snow, fading with the weather as it does outdoors.
- Through a city gate, `DrawMainPrecipitation` draws the main view's rain into the doorway's picture;
  that draw is not cut (`Native::SetPrecipitationWhole`).

This needed the particle shader hooked like the lighting, utility and effect shaders
(`NativeMask.cpp`, `HookShader<3>`); the cut is the geometry-shader clip distances, so it is the same
under Community Shaders (whose pixel shaders it leaves alone). If a particle shader's vertex output
can't be matched (or it draws points, not triangles) its shapes are drawn uncut and `drawsUncut`
counts it: rain then falls in the room, as in vanilla. What the fork did (its particle shader's
`discard`) was the same in both directions; it also lit the rain with the street's light, which the
vanilla particle shader doesn't do.

## Doorway daylight (our own pass)

`Render/DoorwayDaylight.cpp`, called from the render hook right after the doorway's picture is
composited (`Render/Frame.cpp`), before first person. One full-screen triangle with a pixel shader
(`Render/DaylightHlsl.h`) that rebuilds each pixel's position from the main depth (the engine's `kMAIN`
depth as a texture, the depth view unbound meanwhile), keeps those on the interior side of a
merged doorway and multiplies the scene colour by `1 + gain` (blend `DEST_COLOR, ONE`, so it needs
no copy of the scene). Nothing of the game's shaders is touched: stock Community Shaders (any
version) and ENB draw the scene as they do, and ENB's grading comes after.

- Sunlight patch: a surface is lit when the ray from it to the sun passes through the doorway's
  rectangle (soft edge, fades out past 1400 units of ray and at grazing sun), tested against the
  depth for what stands in the way inside (8 screen-space samples toward the doorway), times the
  surface's cosine to the sun (from the depth's derivatives). Brightness follows the street's sun
  colour times the outdoors' sunlight scale, nothing at night, in the sun's tint.
- Sky spill: the opening taken as one small source at its centre, cosines at both ends over
  `pi r^2 + area`, in the sky's ambient tint.
- Both ways: the room merged into the street (seen from outside, and the patch on its floor once
  walked in), and the street merged into an interior (the patch on the interior's floor). The sun is
  the engine's own while the player is outdoors, else the weather's colour with the direction last
  seen outdoors, then the formula plus what was learned for that hour (`Core/Lighting/SunCorrection.h`,
  the Sky Sync difference).
- The maths is `Core/Lighting/Daylight.h` (tested in `tests/Core/LookTests.h`); the HLSL repeats it
  and takes its constants from it. Compile it with `tools/check-hlsl.ps1 -Files src/Skyrim/Render/DaylightHlsl.h`
  after any change: the game's compiler reports errors only in the log.

## ENB

`Enb.cpp` (`Enb::Detect`, in `src/Skyrim/Render/`) finds ENB's SDK exports (`ENBGetSDKVersion` in d3d11.dll / dxgi.dll) and
reports version and whether effects are on. ENB works with it: no game shader is
replaced, so ENB's recognition of the game's shaders is untouched, and the mask leaves no depth
where it cuts, which ENB's screen-space effects read. With ENB the engine's context is ENB's proxy;
its function table is hooked the same way (untested: no ENB on the test machine).

What stays: ENB grades interiors and exteriors with separate settings and switches when the
current cell changes, i.e. at the moment of crossing. The view through a doorway is graded as the
space the player is in.

## Tested (AE 1.7.104, Whiterun: Belethor's, Bannered Mare)

- Vanilla shaders: street through Belethor's doorway and the Bannered Mare's room from the street
  both masked cleanly; the portal pass (bMergeOutdoors=0) clipped and lit with the inn's lights;
  mountains through the doorway fogged instead of black.
- Not tested: ENB; a stock Community Shaders; performance on a slow GPU (the cut costs a geometry
  shader on the shapes near the doorway and on every shape of the other space that crosses the
  doorway's edges).
