# Changes to Community Shaders for SeamlessSkyrim

With Community Shaders installed, SeamlessSkyrim needs this patched version of it (without
Community Shaders, on the game's own shaders or ENB, the plugin does the work itself: see
RENDER-BACKENDS.md). The fork lives in
`E:\projects\CommunityShaders-Seamless`, branch `seamless-portal`, based on release **1.9.1**
(commit `265c28f`). The whole patch is one squashed commit on that branch, so
`git diff 265c28f..seamless-portal` is the complete patch and a rebase onto a newer release is the
way to carry it forward.

Published source (GPL-3.0, required because releases ship the patched DLL):
https://github.com/OneManMonkeySquad/CommunityShaders-Seamless, branch `seamless-portal`, which
the local branch tracks (remote `fork`). The patch stays one commit so commit timestamps aren't
published: fold new work into it (`git commit --amend`, or squash) and force-push.

This file says what each change is for, so it can be redone by hand if the code around
it has moved.

Originals of the installed shader files that were overwritten are kept in
`backup/CommunityShaders-1.9.1-original/`.

Installed files that differ from stock 1.9.1:

- `Data/SKSE/Plugins/CommunityShaders.dll` (+ `.pdb`)
- `Data/Shaders/Lighting.hlsl`
- `Data/Shaders/Common/SharedData.hlsli`
- `Data/Shaders/Common/Permutation.hlsli`
- `Data/Shaders/Utility.hlsl`
- `Data/Shaders/ISSAOComposite.hlsl`
- `Data/Shaders/Water.hlsl`
- `Data/Shaders/Particle.hlsl`

Community Shaders keeps compiled shaders in `Data/ShaderCache/<shader>/` and only notices a
change to the shader's *own* file. After changing an include (`Common/*.hlsli`), delete the cache
folders of the shaders that include it (`Data/ShaderCache/Lighting`, `.../Utility`), or the old
code keeps running. This cost a long debugging session once. After the cache is cleared the
Lighting pixel shaders take minutes to compile in the background (about 10 s each, queued);
until then the game runs without them and a test shows nothing of the patch.

## DLL

Exports used by SeamlessSkyrim (looked up with `GetProcAddress`; absent = feature off; the plugin's
list of them, with signatures, is `SEAMLESS_FORK_EXPORTS` in `src/Skyrim/Render/Wire.h`):

| Export | Purpose |
|---|---|
| `SeamlessPortal_BeginPass(NiDirectionalLight*, bool interior)` / `SeamlessPortal_EndPass()` | Around the separate portal pass: per-frame shared data (directional light, ambient SH, `InInterior`, no directional shadows) and the feature buffer describe the space behind the door, then the world's data is put back. Also starts precompiling the composite shaders. |
| `SeamlessPortal_SetPortalShadows(BSShadowDirectionalLight*, ID3D11ShaderResourceView* shadowMaps, ID3D11ShaderResourceView* volumetricShadowMaps)` | The sun's shadows of the outdoor space the next portal passes draw: the plugin ran that space's own sun shadow light with the portal camera into shadow map arrays made like `kSHADOWMAPS_ESRAM` and `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` (`State::portalShadows`); `nullptr` = none. `BeginPortalSharedData` then sets `HasDirectionalShadows`, uploads the light to t98 (`Deferred::UploadShadowLight`, split out of `CopyShadowLightData`) and builds the VSM from those arrays (`VolumetricShadows::Build`, split out of `CopyShadowLightData`, into the same textures); `EndPortalSharedData` uploads the world's again and rebuilds the world's VSM from the view `CopyShadowLightData` last read (`VolumetricShadows::RestoreWorld`). Without it, the low sun's specular glinted on trees through a city gate that the other side's cliffs shade. |
| `SeamlessPortal_SetPortalCamera(float near, float far)` | The near and far distance of the camera the next portal passes draw with (`State::portalDepthRange`); `BeginPortalSharedData` puts them in `SharedData::CameraData`, which turns depth back into distance (`GetScreenDepth`). The world camera's were left there while `CameraViewProj` was the portal camera's, whose near plane stands at a city gate's far gate: VSM's cascade choice (`GetVSMShadow2D`) saw every pixel at a fraction of its distance, and the shadows through Whiterun's gate changed as the player walked up to it. |
| `SeamlessPortal_SetPortalWorldspace(TESWorldSpace*)` | The outdoor worldspace the portal pass draws (behind a city gate), kept until changed; `nullptr` = none (`TerrainShadows::portalWorldspace`). Terrain Shadows loads and updates its heightmap's shadows when the player's worldspace has no heightmap of its own (a city's), and the portal pass's feature data describes it (`GetCommonBufferData` while `portalSharedDataActive`). The low sun behind Solitude's gate was shaded outside by the mountains to the west, not in the gate. |
| `SeamlessPortal_SetMergedRoomDoor(int room, const float[12])` | Render-merge, one of up to four rooms merged at once (`State::seamlessRooms[room]`, 0..3): doorway centre, normal, horizontal axis, half width, half height (world coordinates), and 1 when the camera is in the merged space (room 0 only: `SeamlessDoor::cameraInside`, uploaded as `SeamlessParams.z`); or `nullptr` for "nothing merged behind this door". Room 0 is the one the player can walk into; 1..3 are only seen from the street. |
| `SeamlessPortal_SetMergedDoor(const float[12])` | Room 0's door (as above): what a SeamlessSkyrim that merges one room calls. |
| `SeamlessPortal_SetPortalClip(const float[4])` | Portal pass: the far door's plane in world coordinates (unit normal pointing away from the portal camera, offset), stored in `State::portalClip` and copied into `SharedData::SeamlessPortalClip` by `BeginPortalSharedData`; `nullptr` = none. Pixels with `dot(P, n) < offset` are discarded in a portal pass. |
| `SeamlessPortal_SetMergedRoomLighting(int room, const float[19])` | Render-merge: a merged room's own directional light (3 colour, 3 direction towards the light), directional ambient (3x4 rows), world space, and a scale for the space's lights and ambient (`SeamlessRemoteDirDirection[room].w`, applied in `Lighting.hlsl` as `seamlessLightScale`); `nullptr` = lit like the world. |
| `SeamlessPortal_SetMergedLighting(const float[19])` | Room 0's lighting (as above). |
| `SeamlessPortal_SetInteriorSun(bool)` | The street merged into an interior: Interior Sun's sun light and shadows on (`InteriorSun::seamlessSun`), as for an interior flagged for them; also sets `SeamlessParams.w` so `Lighting.hlsl` applies the sun's shadows to the merged street only. SeamlessSkyrim points the engine's sun shadow light (`ShadowSceneNode::sunShadowDirLight`) at the outdoor sun meanwhile by giving it a directional light of its own (`AimSunShadow` in `src/Skyrim/Portal/MergedView.cpp`). |
| `SeamlessPortal_SetGatePassage(float depth)` | Room 0's doorway is a city gate, the other worldspace drawn by the portal pass up to `depth` beyond it (SeamlessSkyrim: the far gate's distance plus the portal's cells); 0: not a gate (`State::seamlessGatePassage`, uploaded as `SeamlessParams.y`). `SeamlessDoorClips` then masks the world's shapes through room 0's doorway only up to that depth, and the portal pass (`BeginPortalSharedData`) leaves room 0 out. Through Solitude's gate the city's side of the gatehouse, a brick wall, stood where the sky was. |
| `SeamlessPortal_SetMergedFog(const float[10])` | The merged space's fog (near, far, power, most it covers, near colour, far colour): the street merged into an interior gets the weather's, a room merged into the street its cell's own (most it covers 0 for an interior without fog); `nullptr` = fogged like the world. One for all merged rooms. Uploaded as `SeamlessRemoteFogParam/Near/Far` (appended to `SharedData`). Applied in `Lighting.hlsl` to shapes flagged `SeamlessRemote` (forward), and in `ISSAOComposite.hlsl` (the deferred fog) to pixels `SeamlessDoorClips(position, false)` takes from the interior. Without it the interior's fog turned the distant mountains black. |
| `SeamlessPortal_SetFrontFog(const float[10])` | Rooms merged into the street, seen from it: the street's fog (as `SetMergedFog`) for the air between the camera and their doorways, uploaded as `SeamlessFrontFogParam/Near/Far`; `nullptr` = none. `SharedData::SeamlessRoomFog` fogs a room's pixel with the room's fog beyond the doorway and the street's in front of it, combined (`Lighting.hlsl` forward, `ISSAOComposite.hlsl`). Off in the portal pass. |
| `SeamlessPortal_SetMergedRoomDaylight(int room, const float[3])` | A room merged into the street, seen from it: the street's light coming in through its doorway, as the opening's radiance (rgb), `SeamlessRoomDaylight[room]`; `nullptr` = none. `Lighting.hlsl` adds it to the ambient of the room's shapes times the doorway's form factor from the pixel (`SharedData::SeamlessDoorwayView`, unoccluded). 0 in the portal pass. |
| `SeamlessPortal_SettleSkylighting()` | The player walked out of a doorway: for the next 16 probe updates, Skylighting takes each probe at full confidence from its first sample (`SkylightingSettings.SeamlessSettle`, `UpdateProbesCS.hlsl`), stored as confident so the usual accumulation carries on. Its probes are kept by position, the player jumps from the interior's coordinates to the street's, and the usual fade-in (a loading screen hides it) lit the street under roofs and porches as if open to the sky for a second or two. |
| `bool SeamlessPortal_OutdoorSunDirection(float hour, const float[3] vanillaSunPosition, float[3] towardSun)` | The street merged into an interior: the direction toward the sun outdoors at `hour`, as Sky Sync turns it (`SkySync::OutdoorSunDirection`, which repeats `Update`'s windows, the alternate sun path and `ShadowFader`'s elevation clamp without the dusk heading lock). Indoors the sky updates no sun, so SeamlessSkyrim computes vanilla's sun position itself and asks this for the light outdoors would have (`SunDirections::Outdoors` in `src/Skyrim/Render/SpaceLighting.cpp`, through `Render::Backend::OutdoorSunDirection`). False when Sky Sync is off or doesn't place the sun (night); SeamlessSkyrim then uses vanilla's light direction, which by day is about 13 degrees flatter than Sky Sync's. |

SeamlessSkyrim in turn exports functions the fork calls (if present):
`SeamlessPortal_LightVisibility(BSGeometry*, NiLight*)`,
`SeamlessPortal_IsMergedShape(BSGeometry*)` (does this shape belong to a merged space),
`int SeamlessPortal_MergedRoomOf(BSGeometry*)` (which merged room, 0..3, or -1; without it every
merged shape is room 0) and `ShadowSceneNode* SeamlessPortal_MergedRoomScene(int room)` (the light
scene that room's lights are in: rooms merged at once stand in the same place in their own
coordinates and each has its own scene, while only one can be registered in `State` slot 3). They are
defined in `src/Skyrim/Render/ForkExports.cpp`; `SeamlessPortal_IsUnmaskedShape(BSGeometry*)` (below) is there too.

Diagnostics: environment variable `SEAMLESS_MERGE_DEBUG` = `0` treats every shape as the
player's space, `1` every shape as the merged space, `2` turns the masking off.

Changes by file:

- `src/Features/LightLimitFix.cpp/.h`
  - `SetupPortalStrictLights`: passes drawn with the light scene in `BSShaderManager::State` slot 3
    (the portal's) get their strict lights from that scene, nearest first; each light is
    converted (inverse-square lights) *before* its reach is tested, once per scene per frame
    (per shape, that took a third of the main thread with Understone Keep merged). For a merged
    room, lights whose reach doesn't touch the space seen through its doorway are left out for
    the frame. Shadow-casting lights are
    scaled by `SeamlessPortal_LightVisibility` and lose the shadow flag (no shadow maps there).
    Called from `BSLightingShader_SetupGeometry_After`. Also for shapes of a room merged into the
    world (main pass, `seamlessDoor.enabled` and `SeamlessPortal_IsMergedShape`): their lights stay
    in the plugin's slot-3 scene instead of the world's, so they don't light the street or take
    the town's shadow slots; positions use the main pass's cached eye position. Which merged room a
    shape is in is read from the room bits `UpdateLightingShaderPermutation` set for the same pass
    just before (`ExtraShaderDescriptors::SeamlessRoom`), and its lights come from the scene
    `SeamlessPortal_MergedRoomScene` names (slot 3 if the plugin doesn't export it).
- `src/State.cpp/.h`
  - `lastSharedData`, `portalSharedDataActive`, `lastFeatureDataInWorld`,
    `BeginPortalSharedData` / `EndPortalSharedData`; `UpdateSharedData` keeps a copy of what it
    uploads and doesn't upload while a portal pass is active; ambient SH code factored into
    `SetAmbientSH`.
  - `ExtraShaderDescriptors::SeamlessRemote` (bit 7); `UpdateLightingShaderPermutation` sets it
    for shapes `SeamlessPortal_MergedRoomOf` (or `SeamlessPortal_IsMergedShape`) places in a
    merged room while any door is merged, and the room's index in `SeamlessRoom` (bits 11-12,
    `kSeamlessRoomShift`; the word had bits 11 and up free). The utility, effect and water shaders'
    permutations are set the same way from `SetupGeometry` hooks in `src/Hooks.cpp`
    (`WaterExtensions::BSWaterShader_SetupGeometry`: water had no hook, and drew with whatever the
    last lighting pass had set).
  - `SeamlessDoor seamlessRooms[kSeamlessRooms]` (4, `SeamlessAnyRoom()`); `SharedDataCB` gains
    per-room arrays `SeamlessDoorPlane/Center/Right[4]`, `SeamlessRemoteDirColor/DirDirection[4]`,
    `SeamlessRemoteAmbient[12]` and one `SeamlessParams` (filled in `UpdateSharedData`, centres in
    world coordinates; the shader subtracts `FrameBuffer::CameraPosAdjust`, because the CPU side has
    no reliable "current camera"). `BeginPortalSharedData` keeps room 0's doorway only: the portal
    pass draws one space.
- `src/Utils/Game.cpp/.h`: `SetInteriorOverride`; `IsInterior()` honours it (features that switch
  on interior/exterior follow the space being drawn).
- `src/Features/IBL.cpp/.h`: IBL counts as disabled while a portal pass is drawn (otherwise its
  static menu lighting replaces the space's ambient). `computedOutdoors` (set at the end of `Prepass`) and
  `BindForPortal(bool outdoors)`, which binds the IBL textures to PS slots 76/77 (the last outdoor
  sky: the prepass doesn't run indoors, so they keep it) or unbinds them. `State::BeginPortalSharedData`
  calls it with the portal's kind, `EndPortalSharedData` with the world's (`!Util::IsInterior()`).
  `GetCommonBufferData` turns IBL off in a portal pass while `computedOutdoors` is false. Without
  this the street seen through a doorway from indoors was lit without IBL and didn't match the street.
- `src/Deferred.cpp/.h`: `PrecompileComposites` compiles both deferred composite compute shaders
  on a background thread (they were compiled on first use: a ~150 ms stall when walking through a
  door without a loading screen); `compositeLock` guards the two getters. Started from
  `Deferred::SetupResources` (it used to start only with the first portal pass, which the merged
  street no longer uses, so the first walk out still compiled the outdoor one).
- Other helper shaders compiled on first use (`Util::CompileShader`, no disk cache) whose first use
  is the first outdoor frame after loading indoors, a 100-300 ms frame mid-walk without a loading
  screen (main thread sampled):
  - `src/Features/GrassOptimizations.cpp`, `GrassOptimizations/HiZPyramid.cpp/.h`: the culling
    shader and the Hi-Z shaders (`HiZPyramid::CompileShaders`, split out of `Build`) are compiled in
    `SetupResources`.
  - `src/Features/GrassCollision.cpp`: `GetCollisionUpdateCS` in `SetupResources`.
  - `src/Features/ScreenSpaceShadows.cpp/.h`: `PrecompileRaymarch`, from `Prepass` while the
    shadows aren't drawn (indoors), compiles the raymarch shader on a background thread
    (`raymarchLock` guards the getter and `ClearShaderCache`). Not at startup: its sample count
    follows the dynamic resolution, which an upscaler sets only once in game; compiled at startup
    it was compiled again on the first outdoor frame.
- `src/Deferred.cpp/.h`: `UploadShadowLight(light)`, the t98 upload of `CopyShadowLightData` for
  any shadow light. `src/Features/VolumetricShadows.cpp/.h`: `Build(shadowMaps, volumetricShadowMaps)`
  and `RestoreWorld()`, `CopyShadowLightData`'s VSM build for given arrays (see
  `SeamlessPortal_SetPortalShadows`).
- `src/Features/TerrainShadows.cpp/.h`: `portalWorldspace`, `IsHeightMapReady(worldspace)` (see
  `SeamlessPortal_SetPortalWorldspace`).
- `src/Features/SkySync.cpp/.h`: `OutdoorSunDirection`, Sky Sync's sun direction for a given hour
  without a sky in exterior mode (fork commit `57fdbaa`).
- `src/XSEPlugin.cpp`: the exports.
- Interior Sun: a New Game crash fix from the first SeamlessSkyrim commit on the branch
  (`eeed548`), unrelated to portals.

## Shaders

- `Common/Permutation.hlsli`: `ExtraFlags::SeamlessRoomShift = 11` (bits 11-12: which merged room).
  `ExtraFlags::SeamlessRemote = 1 << 7`, `ExtraFlags::SeamlessUnmasked = 1 << 8`
  (same values in `State::ExtraShaderDescriptors`). `State::UpdateLightingShaderPermutation` sets
  `SeamlessUnmasked` for shapes the plugin's `SeamlessPortal_IsUnmaskedShape(BSGeometry*)` names
  (the player's), and the `discard` in `Lighting.hlsl` and `Utility.hlsl` is skipped for them: the
  player walking through a doorway is drawn whole instead of being cut at the door plane.
- `Common/SharedData.hlsli`: after the fog fields, `SeamlessRoomDaylight` and
  `SeamlessFrontFogParam/Near/Far`, with `SeamlessDoorPassed`, `SeamlessRoomFog` and
  `SeamlessDoorwayView` (see the exports above).
- `Common/SharedData.hlsli`: the `Seamless*` fields at the end of the `SharedData` cbuffer, by
  merged room (doors: `SeamlessDoorPlane/Center/Right[4]`, plane `w` 1 while that room is merged;
  lighting: `SeamlessRemoteDirColor[4]`, `SeamlessRemoteDirDirection[4]` with the light scale in
  `w`, `SeamlessRemoteAmbient[12]`, three rows per room), then `SeamlessParams` (`x` any room
  merged, `y` room 0 is a city gate (`SeamlessPortal_SetGatePassage`), `z` camera in room 0, `w`
  sun shadows for the merged street). Must match `SharedDataCB` in `State.h`; float4 arrays pack at 16 bytes per
  element on both sides. Fields after the arrays moved, so every shader reading `Seamless*` fields
  (Lighting, Utility, Effect, ISSAOComposite) must be recompiled: clear those cache folders.
  `SeamlessDoorClips(position, mergedSpace, room = 0)`: the per-pixel rule that masks the spaces
  drawn in one pass against each other: a line of sight
  passes into a room at the first doorway it goes through (from the camera's side, to a point
  beyond the door plane), and a pixel is drawn only if it belongs to the space its line of sight
  has reached. With one room this is the old rule. Which space the camera is in comes from
  `SeamlessParams.z`, not from the camera's side of the door plane: behind the plane is also the
  ground beside the house, where the camera is still outdoors (the world was masked away there and
  the room drawn in its place). With the camera in room 0 only its own doorway counts, so the other
  rooms aren't seen at all.
- `Lighting.hlsl`: first statements of the pixel shader `main`: `seamlessRoom` from the room bits,
  then `discard` when `SeamlessDoorClips(position, ExtraShaderDescriptor & SeamlessRemote,
  seamlessRoom)`; the merged lighting further down reads the arrays at `seamlessRoom`.
  Right after it `seamlessRemote` / `seamlessDirLightDirection` are defined, and every later use
  of the per-geometry `DirLightDirection` in `main` goes through the latter. For a merged shape
  with own lighting: `dirLightColor` from `SeamlessRemoteDirColor`, no world shadow
  (`GetWorldShadow`), `dirDetailedShadow = dirSoftShadow = 1` before the directional light is
  evaluated (not while `SeamlessParams.w` is set: the merged street keeps the sun's shadows), `directionalAmbientColor` from `SeamlessRemoteAmbientFor`, `skylightingDiffuse = 1`. Then the street's light through the doorway is added to it
  (`SeamlessRoomDaylight`, see `SeamlessPortal_SetMergedRoomDaylight`).
  For a merged shape (`ExtraFlags::SeamlessRemote`) the world's clustered lights are skipped in the
  LLF light loop: its strict lights (the room's own) light it alone.
  Also for a merged shape: the IBL diffuse replacement of the ambient is skipped (it lit the room
  with the outdoor sky), lights and ambient are multiplied by `seamlessLightScale`, and
  `psout.Reflectance` is scaled by `0.5 * seamlessLightScale` (outdoor cubemap reflections).
- Both passes take the masked pixel's position from `SharedData::SeamlessPixelPosition(SV_Position)`
  in the main view (the lighting pass falls back to its interpolated position in reflections and
  cubemaps): with different sources the doorway's edge z-fights.
- Portal clip: `SharedData::SeamlessPortalClip` (float4, appended after `SeamlessRemoteAmbient`
  in both `SharedData.hlsli` and `SharedDataCB`) and `SharedData::SeamlessPortalClips(position)`;
  `Lighting.hlsl` and `Utility.hlsl` discard when it says so, using the same position as the door
  mask (SV_Position-derived in the main and portal views). Needed because the plugin's near-plane
  push only clipped correctly when looking straight through a doorway: from close by at an angle,
  the back of the far building's doorway (a closed door) showed in the portal.
- `Utility.hlsl` (depth prepass and friends): the same `discard` in the pixel shader, so masked
  shapes don't leave depth behind. Not into shadow maps: `ExtraFlags::SeamlessLightView` (bit 13),
  set in `UpdateLightingShaderPermutation` when the depth target is a shadow map's
  (`kSHADOWMAPS`, `kSHADOWMAPS_ESRAM`, `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`,
  `kPRECIPITATION_OCCLUSION_MAP`). Not by the technique: the engine draws most statics' main-view
  depth with `RenderShadowmap` (0xC000) too, and those kept their depth through a doorway. The position is rebuilt from
  `SV_Position` with `CameraViewProjInverse`; **do not add vertex shader outputs here**: it shifts
  the vertex shader's rounding, the lighting pass (depth test EQUAL) z-fights with the prepass
  and the ground turns white and scribbled.
- `Utility.hlsl` vertex shader: the paraboloid shadow-map projection (`RENDER_SHADOWMAP_PB`) is
  tested before `LOD_LANDSCAPE`. Distant land drawn inside an interior gets into a point light's
  paraboloid shadow map; in the original order that permutation left `positionCSPerspective`
  undeclared and failed to compile.
- `Lighting.hlsl`: the world shadow (`ShadowSampling::GetWorldShadow`: terrain and cloud shadows)
  also in a portal pass, not only in the world and reflections.
- `Lighting.hlsl`: the Volumetric Shadows lookup (`ShadowSampling::GetLightingShadow`) also in a
  portal pass (`SeamlessPortalClip` set), not only in the world; it has directional shadows only
  with `SeamlessPortal_SetPortalShadows`. Forward, without the engine's shadow mask, its detailed
  value becomes `dirDetailedShadow`, which scales the sun's diffuse and specular.
- `Lighting.hlsl`: with `SeamlessParams.w` set (sun shadows on for the merged street), shapes that
  aren't the merged street get `dirSoftShadow = dirDetailedShadow = 1`: the interior keeps its
  unshadowed directional light.

- `Water.hlsl` pixel shader: the same `discard` (`SeamlessDoorClips`, the water's space from its
  permutation), by the water's own position (`WPosition`, the stencil variant's `WorldPosition`):
  the vertex shader pushes the water's depth (`heightMult`, `HorizonFix`), so the one rebuilt
  from `SV_Position` isn't where the water is. The street's canal, a plane running under the
  houses, showed in the Ratway's doorway over the steps down behind it. SeamlessSkyrim places
  water in room 0 while the street is merged into an interior (the water then is the street's).

- `Particle.hlsl` pixel shader, precipitation (`ENVCUBE`) only: the same `discard`
  (`SeamlessDoorClips`, by `SeamlessPixelPosition`), its space from its permutation, which the
  particle shader's `SetupGeometry` hook in `src/Hooks.cpp` now sets
  (`UpdateLightingShaderPermutation`). SeamlessSkyrim places the weather's rain and snow in room 0
  while the street is merged into an interior: they fall only beyond the doorway. Seen from the
  street they are the world's and don't fall in the merged rooms. Through a city gate (room 0
  with `SeamlessParams.y`) they aren't masked: SeamlessSkyrim draws them into the gate's picture
  too, and where the picture has no far side the world's sky, rain included, shows through. Rain in room 0 with lighting is
  lit by `SeamlessRemoteDirColor` and `SeamlessRemoteAmbientFor`, without IBL and the clustered
  lights (the interior's): lit as the interior it was dark.

Not patched yet (so not masked in a merge): grass, effects and other particles, distant trees, sky.
