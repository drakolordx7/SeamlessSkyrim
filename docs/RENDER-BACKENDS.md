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
into the pieces outside each doorway's X, multiplied; not solved. `[Portals] iShownRooms` above 1
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

Not done: the street's sun shadows inside an interior (the sun's direction is the computed one),
and, for a room seen from across the street, the street's light through its doorway and the
street's fog in front of it.

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
