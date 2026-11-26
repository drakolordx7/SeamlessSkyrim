- SE 1.5.97: `coc MarkarthUnderstoneKeep` from Markarth's street, with the keep merged into the
  street (walked in and out just before), hung the game during the load (once; log ends at the
  `coc`). Not tried on AE.
- AE 1.6.1170: the cause of a fall through the street after a walk-out isn't known; it is worked around
  (`TransitionStalls` SettleHook). Walking out of Belethor's (Whiterun), in about a third of the
  launches (the upstream plugin too), the engine's settle step after the arrival
  (ID 40744 + 0x53B, a loop calling ID 37507 with a 0.16 s step) moved the player 70 units down
  at the first call and on to the land 146 units under the porch (`WhiterunPLainsDistrict03`,
  z -3510 instead of -3364). Found in a good and a bad launch alike: every body moved back before the teleport
  call, the porch's body (ref 0001BF79) in the world and in the player's phantom's list with the same
  filters, the world not locked, no passage open, rays (static layer and the phantom's own filter)
  hit it, the controller's state before the step the same. Putting the phantom into the world
  again before the step didn't help. The settle step is left out on our seamless teleports; not
  hooked on SE and AE 1.7.99+ (the call site is only known for 1.6.1170, so the bug may stay there).

- Frame hitches left after the crossing work (Whiterun crawl, 38 doors, the hitch watcher: BUILD.md): around each crossing
  (`in`, `out` stages) 80-90 frames per crawl are 40-90 ms (median frame 10-13 ms), no door over 100 ms except two
  one-frame ones: the main gate walking in from Tamriel (~160 ms: the engine reads Whiterun's grid cells from the plugin
  files, `TESFile::SeekCell` under `TESWorldSpace::LoadCell` in the teleport; `RemoteCell::Prewarm` only runs for a walk
  out of an interior) and Dragonsreach's cellar door walking out (~140 ms, `TESWaterSystem::AddLODWater` in the teleport).
  The rest is spread thin: the portal pass's draws (`PortalPass::DrawPassHook` and the driver waits under it, ~half),
  scene graph walks of ours in the crossing frame (`Engine::ForceFadeIn`, `MovedBounds::UnfixBeforeMove`,
  `SettleMotion`, 40-70 ms), the light update, `Pictures::Cache` creating a layered picture's textures (`gpu-wait`).
  Not cut: each is under 100 ms and was not the cause of the long ones.
- The test machine's GPU is the limit in Whiterun's main street (99% 3D engine use, the world draw 28 ms with `perf` on):
  60-300 ms frames after the crawler's teleports and looking down the street are all in `Present` (the render queue
  full), nothing the plugin does. The crawler leaves its own teleports out of the hitch table.
- `TransitionStalls::QueueWaitHook` (the wait for the task queue at the end of `SetCurrentCell`, ENGINE.md) is hooked on
  AE 1.6.1170 only; SE 1.5.97 and AE 1.7.99+ keep the wait (80-500 ms when loads are queued) until the call sites are found.
- The shaders compiled during a session (35 in a Whiterun crawl, 10-90 ms each on the main thread) are kept in
  `SeamlessSkyrim_Shaders` next to the DLL; the first session after an install, or after a shader's source changes,
  still compiles them in the frame that first needs them (the first door opened: ~70-90 ms).
- AE: `place_go` (`coc RiftenBeggarsRow`) from Riften's street just after walking out of Beggar's
  Row hung the game during the load: no CPU, every thread waiting; the log ends with the room's
  door opened again (`Doors::Open`) after the release. Once.
- The skylight (the indoor stand-in for the sun) in The Drunken Huntsman turns off when seen from outside (jarring).
- Markarth: the market attack at the start of The Forsworn Conspiracy played again, not every
  time (user). Not reproduced: no save from before the quest. Possibly the same lost-item bug fixed with
  `belethors-dropped-item-out-and-in`, or the purge of the city's cells after a gate trip
  (`Travel.cpp`, `PurgeBufferedCells`).
- Physics objects and arrows are not carried through a doorway.
- Physics objects can break.
- The street's particles (Riften's falling leaves) drawn inside an interior it is merged into, in front of the doorway too (Mistveil Keep's hall, looking out). Cause not looked into.
- Understone Keep seen from Markarth's street: frame ~13.7 ms, ~9.6 ms with it hidden. Left: the game drawing it, the light setup per shape with `LightOcclusion::Visibility` (~1.5 ms), our cull (~0.5 ms).
- Walking into Understone Keep: the first frame takes ~110 ms (the teleport ~37 ms, releasing the keep's merge ~15 ms, attaching the street ~16 ms, merging it ~5 ms), the next ~50 ms. Walking out: ~115 ms (the teleport ~25 ms, attaching the keep ~16 ms, the player view ~40 ms), then for a second every other frame ~55 ms (the light-occlusion rays catching up).
- Walking out of Riften's jail into the street, the first time after loading the save: the first frame outside takes 95-185 ms (the teleport 70-100 ms), then a few frames of 50-180 ms. Before the grid was prewarmed, moving the street back out of the jail was 50-270 ms of it, nearly all of it Havok moving the street's bodies (ENGINE.md, "Moving a cell's 3D moves its bodies too"); not split up again since.
- Walking out of Belethor's the first time after loading the save: the first frame ~80 ms (the teleport ~60 ms: the grid cells' attach, and ~20 ms of po3_LightPlacer's work under ID 19411).
- The street merged into Riften's jail right after loading its save: the frame of the merge is drawn nearly black, the room included (seen with and without the persistent references' pass). Cause not looked into.
- Whiterun's gate, walking in from Tamriel right after walking out (from 450 units outside): the player comes out 11 units past the gate and is carried back out 3 s later, standing in its doorway once the view back is ready. Walking in from the gate itself they come out 250 units in and stay. Same at c39335d.
- While a doorway's picture is being captured (up to ~3 s before a room is merged), every capture frame shows the previous frame's picture in the doorway: the view lags a frame while it lasts. Drawing the live pass as well on those frames would fix it, at about double the portal cost while capturing.
- Persistent references without 3D whose cell has no loaded data (still filed under the worldspace's persistent cell) are loaded on the main thread, not queued (`RemoteCell::PersistentIn`): 71 of them around Riften's jail, ~30 ms of a 50 ms attach.
- In the Bannered Mare with the street merged, the world scene's light update takes ~4 ms every frame (`perf`, `lights: world`). Not looked into.
- Whiterun's hanging shop signs: torn off and fallen on walking out of Belethor's, twice in ~30 walk-outs (`belethors-walk-out`; once right after loading the save coming from Riften); cause not found.
- Whiterun's distant objects and trees gone right after walking out of Belethor's (the user had been in and out of it and the Bannered Mare a few times): every LOD node's object and tree layer state 2 (none), the land's normal, the mountains bare low LOD land until a game is loaded. Not reproduced in ~30 walk-outs of either of its doors (front and back), quick or after waiting inside.
- Loading saves one after another across worldspaces (Whiterun's Belethor's, Riften's jail) with portals on crashes now and then (ID 19165 putting an actor's 3D into a cell without 3D), also without the persistent references' 3D loading. During the sectors work (APPROACHES.md, 5), loading a save, travelling to Swindler's Den and loading again crashed with all of that switched off.
- Looking out of an interior built of rooms (Mistveil Keep at Riften), the street's distant tree
  LOD is hidden. It was a workaround for a hang seen with the removed sun-shadow pass (the LOD came
  into the sun's shadow map twice, the batch list looped; ENGINE.md, Culling); whether the hide is
  still needed without that pass isn't tried.
- Walking out of the Smelter Overseer's House (Markarth's mines), first time after loading the
  save: a flat grey wall, like distant LOD, drawn over the pipe and the city behind the house's
  corner. In each of 22 full suite runs (22:05–23:03), then in none of 7 runs after; cause not
  looked into.
- Walking into Understone Keep in the suite (`understone-keep-walk-in-out`, right after the smelter
  house's route): the game was gone once, with no crash log and the plugin log lost to the
  relaunch. Not again in 4 runs of the same two routes. `run_suite` now keeps the log.
- Loading the Smelter Overseer's House's save again 0.4 s after the street had been attached
  (a launch with the save, then the route's own load): 41 of the merged street's shapes drawn at
  the street's own place, not the room's. Once; not in 5 loads after. Cause not looked into.
- `place_go` out of that house with the street merged into it: `coc Wilderness` left the player
  in the house's cell, then moved to the street's coordinates and falling. Once.
- The street merged into an interior stays merged until it is let go, also far from the door
  (2400 units in an interior with few doors): the cost `closeBy` in `PortalManager.cpp` is there
  to avoid (2.6 ms a frame in Honningbrew Meadery's cellar) is only avoided coming from afar.
  Nothing takes a merge back short of letting the space go.
- Outdoors with few doors around, a fourth space coming within reach halves the reach
  (`sparse`), and every kept room beyond the shorter one is let go in the same frame, as is a
  city gate's view beyond it.
- Walking into Whiterun through its gate from outside (route steps: load
  `SeamlessTestWhiterunGateOutside`, walk to (19900, -7424)), the player is carried straight
  back out about 3 s later: the city's arrival point is 40 units past the city-side gate's plane,
  and once the view back out is attached, `Approach` crosses at depth >= 0 without the walk being
  towards the doorway. Seen at 4749361, before the gate reach changes.
- Seen from indoors, the street's sun direction is the engine's from the moment the player came in for half a game hour; then the formula's plus what was learned outdoors for that hour (`SunDirections::LearnOutdoors`, Sky Sync and the like). A session that was never outdoors in those hours with such a mod has the formula's alone, up to ~13 degrees off with Sky Sync. The colours jump to the weather's computed ones at the same time (`Render/SpaceLighting.cpp`, `RecentOutdoors`).
- A door's 3D rebuilt while held open is noticed at the next `Doors::Open` on it; the door the
  player came out of is opened once and waits for another opener.
- Walking out of Solitude's gate and turning round at once: for up to 4 s the gateway shows
  Tamriel's own side of the gatehouse (a brick wall), until the view back is attached after the
  purge (`Travel.cpp`, `purgeDeadline`).
- Through Solitude's gate from the city: the snowy mountains behind the fort, seen once outside,
  aren't there; the city's worldspace's own distance shows where the portal pass drew sky.
- Sven and Hilde's house door in Riverwood (01CBB0): its hand turn (-1.6°) was made against the
  door's teleport marker and not converted when the doorway took the door's panel instead (the
  door was locked in every save at hand). The others were.
- Standing at Solitude's gate in the city (the Battle for Solitude save), both gates lost their 3D
  once, 25 s after walking up, and the portal stopped drawing ("a door has no 3D"); the city behind
  the gate showed. Once; not after a reload. Cause not looked into.
- `coc` out of Solitude's Hall of the Dead with the street merged in: the portal was let go, then
  attached to the Hall's door again before the load, and stayed active and merged in Belethor's
  until a second `coc`. Once.
- Walking out of Belethor's (`belethors-walk-out`): one frame just after the crossing drawn almost
  white, as if overexposed. Once in two runs. Cause not looked into.
- A room merged into a sunlit street gets the street's sunlight patch and sky spill (`Render/DoorwayDaylight`), but not the street's fog in front of the doorway, and the patch knows nothing of what shades the street (an eave, the house opposite): the patch is drawn whenever the sun is up and the weather's sun colour is bright. Pictures of rooms (stand-ins, tiers) get none of it.
- Whiterun, after walking into Arcadia's Cauldron and out, then saving at night ~600 units away:
  Arcadia's and Belethor's doors stood open on the house's backing panel, with no portal activated
  again, even at the door; seven other doors stuck as open stand-ins since the walk out. Not
  reproduced with a console save after the same walk; how the user saved isn't known yet.
- Riften's door to the docks (000422AD, portal switched off): walking back in from the docks, the
  player stops 31 units short of the doorway at 000FE264, a reference without a model in
  RiftenCityWest that `RemoteCell::ClearPassage` leaves in the passage (it goes by the 3D's bound;
  not confirmed to be why).
- Riften's north gate: with the small door beside it (00078EDA, portal switched off) on as well,
  that door keeps the portal and the gate stays shut, even standing at the gate (both lead into
  the same cell; `kSwitchMargin`).
- NPCs opening a shut door (`DoorUse`, from NpcTravel's door idle hook) seen in the log only
  (Whiterun's market with every door forced shut: they open, NPCs walk through), not watched.
- NPCs walked through the Bannered Mare's doorway from the street (`bannered-mare-npcs-evening`
  with capture): one vanishes a step past the door plane while the one beside it, not yet past,
  is still drawn (`MergedOverrides` hands it to the room there). Seen before the walk changes too.
- An NPC following close behind another through a door can be moved in by the engine from more
  than 200 units out (`kDoorRange`): not carried, it vanishes from the steps and appears at the
  marker inside. Once (Brenuin) the engine moved it straight back out, where it turned round and
  walked in again by the door. Seen once each.
- The first NPC walked through a door after a load can still stand 1.5 s on the marker before
  walking on; the others go on within 0.25 s.
- Shutting a door by using it while standing in its doorway swings it shut onto the player.
- A door pair placed at two sizes is evened out only for the street seen from inside (drawn
  larger, `PlacementOf`). Between two interiors (the portal pass) the far side's wall still shows
  in a band inside the larger doorway.
- Walking out of Understone Keep (`markarth-understone-walk-out`): in the last frame before the
  crossing the guard beside the door was drawn without his head, and the banners on the pillars
  were missing from inside throughout. Not compared with a build before the street was scaled.
- Beggar's Row's street door in Riften, opened and shut quickly many times (the user): then the
  doorway couldn't be walked into, three tries. Not reproduced by the user afterwards, nor in
  ~10 tries of 8–10 uses 0.05–0.6 s apart, from 100 units off or standing in the doorway.
- A door opened for the first time, the space behind it loaded: the user saw it swing open twice.
  Not seen in frame captures at Beggar's Row's street door, opened before its room was there
  (stand-in, merged 4 s later) or after; nor at Elgrim's Elixirs' door.
- A locked door opened with its key: the engine's own trip with its loading screen (only a picked
  lock is caught: the door then opens). Not tried.
- The game's memory grows by about 2.5 GB with each round trip through Riften's south gate
  (private bytes 15 GB after loading, 31 GB after ten trips; the crashes after many trips came
  with the machine's memory nearly used up). Not SeamlessSkyrim's portals: the same with the
  engine's own trip and its loading screen, the portal hooks not installed, and with Community
  Shaders, Faster Textures, Engine Fixes' texture patch and 88 of the 91 SKSE plugins removed
  (left: SeamlessSkyrim for the bridge, trainwreck, HDT-SMP; without HDT-SMP this load order
  crashes on faces at load, ID 69192). What grows are GPU textures (bridge `gpu_live`): ones the
  engine still holds through materials after leaving for an empty cell and `pcb`, and ones whose
  `NiSourceTexture` and renderer texture are already freed but whose D3D texture and view stay
  alive with no outside reference, also after `ClearState` and `Flush`. Not yet tried: the game
  without SKSE.
- Crash in the portal pass (2026-11-23, after walking back and forth through Riften's south gate):
  `BSEffectShader` setup (SkyrimSE.exe+155757A, ID 107527+0x4EA) reads a light of the pass whose
  `NiLight` is gone (`BSLight::light` null), drawing the street behind the gate on the first frame
  it was ready. Not reproduced in about 30 trips since.
- Through a city gate the view back takes about 5 s (the purge after the trip, then the load):
  walking back through it sooner takes the engine's trip with its loading screen.
- Water isn't masked at a merged room's doorway: the street's canal shows in the Ratway's doorway
  over the steps down. `NativeMask` hooks the lighting, utility and effect shaders only; the water
  shader isn't tried.
- College of Winterhold, Hall of the Elements: with the door up to the Arcanaeum (000175DD) used and
  open, the walk in (`winterhold-arcanaeum-walk-in`) stops at x 320 instead of going up the stairs;
  the route predates doors opening only when used and doesn't use the door.
- The street merged into an interior: the land of some of its cells is left at the street's own
  place, not moved to the doorway (`scene_check`): loaded inside The Drunken Huntsman
  (`SeamlessTestDrunkenHuntsmanInside`, 3 of 9 cells), walked into Warmaiden's
  (`warmaidens-back-door-walk-in`). Not looked at on screen.
- Whiterun, Dragonsreach (`01A704` front door, `09A160` Jarl's Quarters): the crawler's straight walk from 6 m is stopped by the stairs and rails below the door and a teleport to the door's step doesn't end in the doorway either; not a scheduler fault (the old crawl timed out on both). Not looked into further: needs a real route to the door (`route_save`).
- A room released while the engine's job threads update its actors crashed the game once (`bhkCharProxyController` null vtable in `Actor::UpdateMovement`, Jorrvaskr's NPCs, right after a different room was let go). Not reproduced since rooms are held 8 s and not let go and re-attached within 4 s (`kMinHoldSeconds`, `kReattachSeconds`), but a release while actors update is still unguarded.

- **Dormant (T3) spaces and process levels are not seen in game yet.** A kept space no longer raises its NPCs (`RemoteCell::SettleActors`); built, not run. Not established: which level the engine leaves them at after its own attach (ENGINE.md, "Actor process levels"), and whether `MoveToMiddleHigh` / `MovetoMiddleLow` / `MovetoLow` on a live actor mid-animation leaves a trace when a room is paused again (a door shut with the room still held). The bridge's `actors` per space says what happened. Lowering while the jobs run is avoided (end of frame); a room released while they update is still unguarded, as above.
- Whiterun crawl (`batch6`): the game crashed once 4 s after the Temple of Kynareth was merged (`SkyrimSE.exe+0D3CA22`, a texture stream read on a loader thread, `WRTempleWall01_n.dds`, null vtable call). Not reproduced in 3 runs of the same door.
- Whiterun crawl: the Underforge's walk out (`01A701`) ended in the Underforge twice in a row with an intermediate build (every pass of the late-piece settling saw the street's loose items as out of place); fine since. Earlier crawls also timed out or ended wrong-cell there now and then.

- The low-pass on a doorway's sound (`Audio/DoorwayFilter`) is verified by what the voices read back (`portal_status.audio.playing.voices[].filter`: a 32 kHz street marker seen from inside Bannered Mare's door reads a low-pass of 3.8 kHz), not by ear. XAudio2's filter reaches a sixth of a sound's own sample rate and no further, so an 8 kHz ambient loop (the Mare's own) takes it only when the cutoff falls under 1.3 kHz, which a doorway's geometry rarely gets to: such loops are quieter, not duller. The scale (`Tuning::clearCutoff` 7000, `mutedCutoff` 500) was chosen by the maths, not listened to.

## Look features lost with the removed Community Shaders fork, by how much a player notices them

Rebuilt (what is left of them is in RENDER-BACKENDS.md): rain and snow at the doorway, the street's daylight through an open door (now shaded by what the camera sees of the street), the street's sun shadow seen from inside an interior, Sky Sync's sun direction (learned), side rooms in parallax (doors of the merged street show their depth image). Not rebuilt, in the order they matter:

- **TODO The other worldspace's sun shadows through a city gate.** The portal pass draws the other worldspace with the engine's sun shadow mask, which belongs to the main view; the real thing was the fork's shadow-map feed (`SetPortalShadows`). A screen-space pass like the street's (`Render/DoorwayDaylight`, `PSShadow`) over the picture's own depth would do it, but the portal target's depth is linearised with the portal camera's own near and far (`Pictures/Hlsl.h`, `kViewDistance`), and whether the mask already darkened those pixels isn't known: drawn blind it could shade twice. Matters at a low sun through Whiterun's and Solitude's gates (the picture's trees glint where cliffs should shade them).
- **Check, was a TODO: the street's distance fog in front of a room seen from across a foggy street.** Probably already there: the SSAO composite (ID 107245) fogs every geometry pixel by view depth with the active scene's fog, which outdoors is the street's, so a merged room's pixels are fogged by the street's fog for their whole depth (the air up to the doorway included; the room's part too, which is slightly too much). A second fog pass would fog twice, so none was built. What to look at: a room seen from across the street in heavy fog should be as hazy as the wall around its doorway; if it is crisp, the composite skips merged pixels and a pass like `DoorwayDaylight` fogging by the distance to the doorway plane is the fix. Inside, the street through the doorway is fogged by the street's fog (`CompositeFogHook`), the interior's own fog for the air in front of the doorway is missing (rarely any).
- **The daylight patch and the street's shade are made from what the camera sees**: occluders out of the frame (behind the camera when looking at a house from the street) shade nothing. Seen from outside, the eave over the door and the house opposite shade the patch only while they are on screen.
- **TODO Skylighting settling on walking out.** Community Shaders' Skylighting probes are kept by position and the player jumps from the interior's coordinates to the street's, so for a second or two the street under porches is lit as if open to the sky. It is a probe cache inside Community Shaders (the fork forced its confidence); a plugin alone can't reach it, and with no CS patch there is no cheap stand-in (holding the doorway's picture longer hides it at the cost of a late reveal; darkening under roofs in our own pass needs the roof geometry). Left.
