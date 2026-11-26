# Engine facts

Skyrim AE 1.7.104, IDs from the Address Library. These were expensive to find. Add new ones in
the same change that needed them.

## SE 1.5.97 against AE 1.7.104

Found porting the plugin; each was checked in the code of both.

- Mostly the same code at different offsets. Where the compiler inlined differently:
  `BGSTerrainManager::Update` (SE 31017 / AE 31802) calls the node placement (31041 / 31828) once,
  where AE calls it three times; SE has the placement's body (a call to 31048 / 31835 with flags
  7 or 0x27, then three vcalls) inlined at the other two places.
- `Renderer::SetRenderTarget` AE 77453 is SE 75646, a wrapper that passes the Renderer object
  (ID 524907, AE 411393) and jumps to 75654. CommonLib pairs 77453 with 75654, which takes `this`
  in rcx; AE's 77453 ignores rcx and loads 411393 itself. Either way the object a caller passes is
  unused.
- Player layout: the actor cull's drawn bits at +0x274 (AE +0x27C); the teleport's queue at
  +0x640 and its valid flag at +0x682 (AE +0x650, +0x692). TES's worldspace at +0x140 (AE +0x148).
- Same in both: fog property fields (+0x38..+0x88), task pool (vfunc 0x80, +0xE0, +0xF0), the
  grid array (+0x10 size, +0x18 cells), ShadowSceneNode allocation 0x308, the Havok step call
  (`call [rax+0x190]` at +0x18 of SE 18440 / 18466), the actor cull's first 19 bytes, culling
  process vtable slots.
- ID 19384 (SE 18972), the reference fade update, takes the reference's 3D
  (`TESObjectREFR::Get3D2`, vfunc 0x70, at every call site), not the reference.

## AE 1.6.1170 against AE 1.7.104

- Same IDs (all of the plugin's exist in 1.6.1170's table); 5 of 33 hook sites at other offsets
  (`Hooks::Offset`), the rest identical, and so are the byte patterns the plugin checks.
- From 1.7.99 the player's runtime data sits 8 bytes later (CommonLib's
  `RUNTIME_SSE_1_7_99`): the teleport's queue at +0x648 and valid flag at +0x68A on 1.6.1170.

## Cells and spaces

- **One current space.** The engine runs either one interior (`TES::interiorCell`) or one exterior
  grid (`TES::gridCells` plus LOD). Rendering, lighting, AI, Havok stepping, sky and weather all
  follow it. Everything here is about keeping a second space alive beside it.
- **Leaving a cell** detaches it, and its actors leave high process. Its references stay in
  memory.
  - The first probe (Dragonsreach → street → Bannered Mare) saw cells drop to `DataLoaded` with
    their 3D freed.
  - Interiors in the interior buffer have since been seen keeping their 3D (state Unattached,
    3D yes), and an attach then has nothing to build.
- **Preloading:** the engine loads the data of interiors linked from nearby doors
  (`bPreloadLinkedInteriors`), so exterior → interior targets are in memory early.
- **Interior → exterior:** the linked door is a persistent reference with no parent cell until
  the exterior loads, so its cell has to be resolved and loaded by us.
- **Attaching a cell that isn't current** (reversed from `TES::SetCurrentCell`):
  - ID 19111 (cell) loads its temporary references.
  - ID 18976 (cell, node) attaches it under any parent node; `SetCurrentCell` passes
    `TES::objRoot`. The cell gets its own `bhkWorld`.
  - ID 19063 (cell, true) finishes: state Attached, actors in high process with 3D. Its gate
    (ID 13308) only checks the cell's state, not whether the cell is current.
  - The attach also installs the cell's room graph in the world light scene
    (`ShadowSceneNode::SetPortalGraph`, slot 0), and the main view then culls with the other
    cell's rooms (actors vanish, crashes). `AttachGraphHook` (`Spaces/EngineHooks.cpp`) skips that
    call for our attaches; `RemoteCell::AttachOne` still sets the world's graph again afterwards,
    which is what undid it before the hook and what holds where the hook's call site isn't found.
- **Detaching:** take the cell's 3D off our root first, then call ID 19130 (cell, true).
  Detached with the 3D still under our root, it took `objRoot`'s first child instead, the
  temporary node manager (`TES+0x98`). The game then crashed in its update (`NiAVObject::Update`
  from ID 13292).
- **Interior buffer** (`TES+0xC8`): detaching puts a cell at its front, and the one falling off
  the end is unloaded by ID 13721. ID 13721 has about 25 callers, and a cell we still hold must
  not go through any of them: hooked at its entry (`InstallUnloadHook` in `Spaces/EngineHooks.cpp`). A held cell
  unloaded the same way crashed in ID 13292.
  - **The buffer lists every loaded interior, the player's own room too** (ID 13434 adds a cell when it is loaded, ID 13435
    puts it or moves it to the front; its size is a global read at ID 13435 +0xF, 3 in the dump read here). Cells we attach and let go
    behind the doors of the player's room (Palace of the Kings: Bloodworks, Upstairs01, Upstairs02 for their pictures) push
    the player's room to the end; the next detach of a cell not yet in the buffer unloads it (ID 13721 -> ID 19056 ->
    ID 19170). ID 19170 sets the world scene's portal graph to null when the cell's graph is the scene's, which it is
    for the player's room: the actor cull of the same frame (ID 40335 -> ID 106433, which reads
    `ShadowSceneNode+0x228` unchecked) crashed on the main thread (Windhelm, Palace of the Kings front door 0C0E2D), and
    the freed room crashed an engine worker thread the same ms (0C0E2E). Not a race: the main thread did both, and
    the crash log's `Render::Frame::ShowActor` frames are the actor cull hook's trampoline mislabelled by the PDB.
    The unload hook leaves the player's room (`TES::interiorCell`) loaded like a held cell.
  - **Exterior cells take the same road.** The buffer for exteriors (`TES+0xD0`, ID 13434's other branch) holds 36 cells
    (5 x 5 grid in the dump read here), a cell pushed off its end is unloaded by the same ID 13721. The unload hook
    only keeps held cells and the player's interior; no exterior cell the engine is using is kept.
  - **Winterhold College** (Midden01 is the player's room, `0002BCD7`): Hall of Countenance and Midden02 attached for their
    pictures and let go pushed the room out; the culling job (`BSGeometryListCullingProcess`, ID 76561) read
    `BSCullingProcess::portalGraphEntry->portalGraph` (+0x30190 -> +0x10) as null and died at `+0x58`
    (`alwaysRenderChildren`). The hook's refusal line appeared at the moment of the crash in the repeat run and the game
    went on: covered by the Windhelm fix.
  - **The wait for the frame's jobs can't be called from the player's update.** ID 35409 (`WaitForSingleObject(INFINITE)`
    on the frame's job event, state `[+0x6C]`, the engine calls it at the end of Main::Update) called before ID 19130 from
    the portal update hung the game at the first release: the player's update is itself part of what is waited for.
    Detaching at the end of the frame instead (queue, run after the wait) hung it after merged crossings too (the arrival
    finished, then the main thread stopped); not looked into.
    Seen in the log (Windhelm crawl, pictures of the palace's inner doors cleared so they were taken again): the hook refused
    the palace exactly when Upstairs02 was let go after its pictures. On walking out, `TES::interiorCell` still points to the
    room (state Detaching) when ID 13316 detaches it into the buffer, and the cell that falls off then is another one
    (Upstairs01): the room being left is not refused and sits at the buffer's front.
- **Persistent references** (a town's houses, porches, paving) belong to the worldspace's
  persistent cell. The grid hangs their 3D under the cell they stand in, and takes it off some
  frames after the player goes inside.
  - Loaded at game start, the save has them with 3D.
  - Loaded indoors after the player was outdoors in the same session, those around the door
    have no 3D, so there is no collision under the door either. `RemoteCell::PersistentIn` queues
    them, but not doors. Launched with a save in Tamriel, so do WhiterunWorld's (137 at its gate).
  - **Queuing a reference's 3D** as `TESObjectREFR::Enable` (ID 19800) does: ID 13057 (`*ID 400332`
    the model loader, ref, priority ID 13321 (TES, its cell, 0), false); read on the loader
    threads, set on the main thread. It counts the reference in its cell's
    `loadedData->queuedRefCount` (ID 19151): a cell without loaded data (a reference still filed
    under the persistent cell) crashed there. Both ends of the task (`QueuedReference` vtable
    slot 12, ID 13022, which sets the 3D, and slot 20 → ID 13023) count it off again (ID 19152),
    with 3D or without, so a reference whose model doesn't load doesn't hold the cell's count up.
    `Load3D(true)` is not a request to queue: it is what the loader threads call, and parks the
    3D in thread-local storage. `Load3D(false)` on the main thread: ~6 ms each from disk.
  - 3D loaded while its cell isn't attached (`Load3D`, also `Set3D` again) hangs nowhere, and the
    engine's later attach of the cell leaves it so: it hangs only the 3D it loads itself. ID 19165
    (cell, ref, true, false) hangs one reference's 3D into its cell, as the cell attach does for
    each (ID 18976 → ID 19187); the cell must have its 3D. Also after the engine's own trip with
    its loading screen: a street let go just before it came back without them (Riften's canal
    walk outside Beggar's Row, the player in the canal).
  - Walking out once, Markarth's ground statics (`MrkDocksideLand01`, temporary references of
    MarkarthMines) became persistent references of a runtime cell `FF000A96 'Wilderness'`, whose 3D
    hangs under `TES::objRoot` outdoors. Why isn't known.
- **A door's teleport marker isn't always square to the door.** Solitude's gate, city side:
  48° off for one of the two load-door pairs standing there (00101D9D/00101D9E), 1.4° for the
  other (00037F1B/0001C386). Markarth's Smelter Overseer's House, street side: 17°. Whiterun's
  shops: 4-11°. Most doors: under 1°. The doorway mapping takes each door's facing from its panel
  and only the side from the marker (`Doors::Compute`).
- **City gates are animated doors.** Their meshes have two leaves turned by a
  `NiMultiTargetTransformController`, like a house door: Whiterun's `WRDoorMainGate01.nif`
  (`FrontGateDoor06`/`08`), Riften's `RTMainGate01.nif` (`DoorL`/`DoorR`), Solitude's
  `SGateDoor.nif` (`SGateDoorLeft`/`Right`).
- **A door's mesh can hold the wall around its panel.** Riften's `RiftenRWDoorLoad01` (base
  0006F267, e.g. Elgrim's Elixirs) carries the stone frame filling the room's arch, wider than the
  panel; the room's own wall has an arch-shaped hole. Hiding a door means hiding its leaves (the
  animated nodes), not its whole 3D: hidden whole, the doorway was a hole into the void.
- **Behind a city gate the player's own worldspace has a stand-in** for the other side, seen
  through the gateway once the gate is gone: Tamriel holds simple copies of Whiterun's and
  Riften's houses behind their gates, and Whiterun's worldspace its land beyond. Solitude's
  gatehouse is walled up behind its gate on both sides (`SMainGate.nif` in Tamriel).
- **A door's swing is its NIF's `Open` and `Close` sequences** (on the 3D root's
  `NiControllerManager`). An interpolator can be sampled at any key time with
  `NiInterpolator::Update1` into an `NiQuatTransform` (translate, quaternion, scale; CommonLib
  only declares it). Whiterun's house doors: 0.6 s each; `Open` stands still for the first
  0.15 s, then eases in and out; `Close` eases in and out. Portal doors replay them
  (`Doors::Swing`, curves sampled by `SwingCurvesOf`), turned by the pair's own angle and towards the side they open into.
- **Sound heard through a doorway** (`Audio/DoorwayAudio`, `Audio/DoorwayFilter`): a cell's sound sources are
  `TESSound` marker refs (`descriptor`), `TESObjectACTI::soundLoop`, `TESObjectLIGH::sound` and its
  `ExtraCellAcousticSpace` (`BGSAcousticSpace::loopingSound`); an exterior's weather loops are
  `Sky::currentWeather->sounds`. `BSSoundHandle` calls are messages to the audio thread (fine from the main thread);
  `FadeInPlay` / `FadeOutAndRelease` take milliseconds. The acoustic space and music are set on the player's cell
  change, so attaching a remote cell leaves them alone. Run in game (2026-11-25, read back through the bridge, not listened to): the loop and the markers play from
  the doorway; the engine plays none of the room's own sounds at the merged place (bridge `sounds`: no second voice with
  the same file), so nothing doubles.
- **`BSAudioManager::GetSoundHandle` returns false while the descriptor's conditions are false.** A sound descriptor
  (SNDR, `BGSSoundDescriptorForm::soundDescriptor`, a `BGSStandardSoundDef`) can carry conditions
  (`BGSStandardSoundDef::conditions`, CTDA). The Bannered Mare's crowd loop `AMBTavernInteriorA2DLPSD` (00100F8D,
  the room's only sound marker) has `GetCurrentTime <= 2 OR >= 20`: by day the engine hands out no handle, at 22:00 it
  does (same call, same descriptor, same flags 0x1A; found by running at 12:00 and at 22:00). The form is the right type, the
  sound has a file, an output model and a category; nothing is wrong with the call. `TESCondition::IsTrue(player, player)`
  gives the same answer, so a refusal with false conditions is a sound that isn't due, not a failure (the bridge shows
  `blockedStarts` against `failedStarts`). The file's hash (`sounds`) is `596B204F` for that loop. Dig into a descriptor
  with Skyrim.esm's SNDR record (EDID, ANAM file, ONAM output model, CTDA conditions, LNAM loop flags) before suspecting
  the engine call.
- **XAudio2 under BSXAudio2 is the 2.7 interface** (CommonLib's `IXAudio2`: `GetDeviceCount`, `Initialize`) and the game makes
  every source voice with flags 0, so no `XAUDIO2_VOICE_USEFILTER`: `SetFilterParameters` on such a voice does nothing.
  `IXAudio2::CreateSourceVoice` is slot 8 of the `IXAudio2` object's vtable (`BSXAudio2Audio::XAudio`); adding the flag
  there gives every voice a filter, whose default is a low-pass at a sixth of the voice's sample rate (it would dull every
  sound), so each voice is set to a high-pass at 0, which passes everything. The filter's frequency is
  `2 sin(pi f / rate)`, at most 1, with the rate of the voice's input (`GetVoiceDetails().InputSampleRate`: the ambient loops
  are 8 kHz stereo, so their filter reaches 1.3 kHz at most); it can't leave a sound as it is above that, so a cutoff higher than
  a sixth of the rate has to be a pass-through. `BSXAudio2GameSound::sourceVoice` (+0x128) is only safe to touch on the audio
  thread: `BSXAudio2GameSound::Update` (vtable slot 0xE of `VTABLE_BSXAudio2GameSound[0]`) runs there once per sound per tick
  (hundreds of thousands of calls a minute), and so does every change of `BSAudioManager::activeSounds`, which is why the
  lookup of a handle's `soundID` is done in that hook. `GetFilterParameters` shows a change one pass late.
  Static reading of `SkyrimSE.exe` 1.6.1170 in the MO2 instance fails (`.text` is Steam-DRM encrypted; `retools` prints
  garbage), so the sound code was probed at runtime instead.
- **A door's open and close sounds** are its base form's `TESObjectDOOR::openSound` and
  `closeSound`; `BSAudioManager::GetSoundHandle`, then `SetPosition`/`SetObjectToFollow`, `Play`.
- **A door's model isn't always upright.** Castle Dour's doors (`SDoorJail02Load.nif`, 00025199
  and 000223B2) are modelled lying down, y up; their references are turned 90° about x to stand
  them up. The door code works in the model turned so its z is up (`Upright` in `Doors/Geometry.cpp`).
  Bridge `ref_3d` prints each node's local rotation.
- **Land meshes** are built only when a cell is loaded into the engine's own grid (IDs 13285 and
  13427 call it right after 19063). A remote cell needs it called itself (`RemoteCell::AttachOne`, `Spaces/Attach.cpp`).

- **A worldspace's room graph and multi-bound map exist only once it has been the current worldspace.** `TESWorldSpace` +0x130
  (`portalGraph`), +0x138 and +0x140 (the multi-bound map the references of its rooms are filed in) and +0x148 (`multiBoundNode`) are
  made by ID 20514 (called first in `TES::SetWorldSpace`, ID 13315, and lazily at the end of the cell attach, ID 18978 +0xD2C).
  The attach reads the map earlier, per reference of a room (ID 18978 +0xB39 -> ID 19191 -> ID 19437 -> ID 19486 through ID 19439's
  accessor `cell+0x128 -> worldspace +0x138`): a cell of a worldspace the player isn't in, with a room in it, crashed on the null
  map (Blackreach's door to Alftand Cathedral, `AlftandWorldOrigin` 0006987F, reference 000C3D08; crash logs of crawl `interiors-1`).
  `Engine::PrepareWorldSpace` calls ID 20514 before attaching an exterior cell; it skips when `portalGraph` is there. Its SE ID isn't
  known (no SE address table or dump on this machine): on SE a cell of a worldspace without the graph isn't attached.
- **Weather sounds can name a sound marker.** `TESWeather::sounds[].soundFormID` of Blackreach's weather is 00012EA2, a SOUN
  (`FormType::Sound`), not an SNDR: `LookupByID<BGSSoundDescriptorForm>` gives null. The marker's own `descriptor` is what plays.

## Simulation

- **An actor's 3D is hung back into its cell every frame:** the 3D position update (ID 37995, for
  every actor in high process, on the job threads; +0x279) calls ID 19165 (cell, actor, true, ..),
  which, when the 3D's parent isn't the cell's node for actors (ID 19339), queues the move there
  (ID 19415, a per-thread queue run by ID 19416 → ID 19414 `AttachChild`).
- **Moving a cell's 3D moves its bodies too:** `NiAVObject::Update` of the cell (ID 70539 →
  the collision object's ID 78910 → ID 78178) sets its bodies' transforms in Havok, and the
  broadphase update (IDs 61802, 64613) is most of moving Riften's street out of the jail on
  walking out (70-270 ms). `NiUpdateData::kDisableCollision` doesn't skip it; ID 78910 returns early
  when a thread-local state (TLS + 0x9E8) is 1, set by whom isn't known.
- **An update with a loose item's collision active reads its place back from its body** (ID 70539
  → 78177 → 78382 → ID 19826). If the body is outside the item's cell (in an interior, below its
  floor), ID 19826 treats the item as fallen out and moves it to another cell (ID 19799). A
  created item (FF id) has its form ID cleared there (ID 36459) and isn't saved. Put a moved cell
  back with its loose items stilled, and give them their flags back afterwards (`Release`).
- **High process:** `Actor::MoveToHigh()` on a remote cell's actors is accepted, and they aren't
  demoted while the cell stays attached.
- **Actor process levels** (`RE::PROCESS_TYPE`: high 0, middle high 1, middle low 2, low 3, none -1; an actor's
  `currentProcess->processLevel`, `Actor::GetProcessLevel()`):
  - `ProcessLists` keeps four handle arrays, `highActorHandles`, `middleHighActorHandles`, `middleLowActorHandles`,
    `lowActorHandles`, with per-list switches (`processHigh/MHigh/MLow/Low`) and a round-robin index for each lower list
    (`currentLowActor`, `currentMiddleHighActor`, `currentMiddleLowActor`): the lower lists are worked through a few actors a
    frame, the high list every frame. An `AIProcess` has `middleHigh` and `high` data only while it is at that level
    (`process->high` is null below high; NpcTravel and NoFades read it for that), so the high process is also where the
    animation sequencer, the fade state and the movement live.
  - The moves between the lists are `Actor` virtuals: `MoveToHigh` (0xF1), `MovetoLow` (0xF2), `MovetoMiddleLow` (0xF3),
    `MoveToMiddleHigh` (0xF4), and `TryMoveToMiddleLow` (0x92; the engine's own try, which can refuse). The plugin only uses the
    first four.
  - Low-process people in a remote cell move in a jump every few seconds and don't animate (AdoptPersistent's note); that is
    what a vanilla actor in a loaded cell the player isn't in looks like, and what a dormant (T3) space shows now.
  - Job threads: the actors of the high list are updated on the engine's job threads (movement, 3D position). Raising or lowering
    one while they run is a race (a released room crashed in `bhkCharProxyController` on a null vtable inside
    `Actor::UpdateMovement`, docs/KNOWN-ISSUES.md). The plugin moves actors only in `Travel`'s end-of-frame call (Main::Update
    after the wait for the jobs).
  - Not established, no way to look: whether the engine's own attach (ID 19063) puts an actor in the high process by itself
    (the notes above say so; the code that skips `MoveToHigh` for pictures assumes not), and which level an actor of a freshly
    attached remote cell has. The exe on disk is encrypted and the game is not run for this, and the web search returned
    nothing but CommonLib's headers (`ProcessLists`, `ProcessType`, `Actor`), so the bridge reports each space's people per level
    (`warm.spaces[].actors`, `schedule.kept[].actors`, `shownActors`) for the crawler to settle it.
- **A space's people follow its tier** (`RemoteCell::SettleActors`, `Spaces/Processes.cpp`): paused (T3, or a shown room whose
  door is shut) it is dormant: no `MoveToHigh`, no Havok step (also while it loads), the engine's level left alone, as for a space
  attached for a picture. Live (T4) the living people of its cells, and the persistent ones of a street, are raised, each
  remembered with the level it had; paused again, those still in its cells (not the ones carried into the player's cell) are
  moved back with the matching `MoveTo...` call. People already in the high process are neither recorded nor lowered. Both
  moves happen at the end of the frame: a door opening raises a frame later, a shut one lowers at once (the scheduler's 8 s hold
  keeps the space attached, not its people up).
- **An NPC's graph `Speed` is pulled, not set:** each graph update reads it from the actor's
  movement through `BSTAnimationGraphDataChannel<Actor, float, ActorSpeedChannel>` (vtable AE
  207549 / SE 260606, slot 1 `PollChannelUpdateImpl` stores it at +0x18; the actor is at +0x28),
  so `SetGraphVariableFloat("Speed")` is overwritten; an actor moved by `SetPosition` walks in place
  with its legs still. `TurnDelta`'s channel is ActorTurnDeltaChannel (207559). Writes inside
  `Actor::UpdateAnimation` (ID 37361, after its ID 37364) never ran for NPCs.
- **AI after being moved:** an NPC held on a path of our own for a second (`SetPosition` every
  frame) stood 1-2 s at the end before its AI walked on; `EvaluatePackage(true, false)` then gets
  it going within ~0.1 s, except now and then the first one after a load.
- **NPCs through a load door:** one following close behind another is moved through without
  `MovementMessageActivateDoor`'s idle call (the door idle hook doesn't see it), sometimes from
  more than 200 units in front of the door. After the move the AI's path goes on from the door's
  marker.
- **Havok:** the engine steps only the current space's world (`*ID 400440`, in the jobs
  ID 18870 / 18897), with `bhkWorld` vfunc 0x32 (arg 0).
  - Stepping a second world from the main thread, beside that job, corrupted memory: it is
    stepped in the same job, right after the engine's.
  - With Precision loaded, that froze the game; it is stepped at the end of the frame instead.
  - A street's world (seen from indoors) is stepped at the end of the frame in either mode; in
    the job it was never tried.
  - `bhkWorld+0xC701` is the active flag, read only by the step.
- **Collision filter:** `bhkCollisionFilter`'s pair filter is at vtable offset 0x10, its ray
  filter at 0x28. Havok returns `hkBool` through a hidden pointer: a `bool` signature crashed.
- **`bhkWorld::PickObject`** with a char-controller filter still hits bodies on the non-collidable
  layer (15): a door made walkable stops the ray, not the player. A ray started on a surface
  misses it. A house's own collision can close its doorway 2 units behind the door's back face
  (Sadri's Used Wares, Windhelm).
- **The character controller stops about 28 units short of a wall.** A house's entry piece that
  closes its doorway 14 units in front of the door's centre (Whiterun's Ysolda's, Heimskr's, Olava's
  houses and the Stables, walking out) holds the player 42 units short of the plane, so a passage
  opened only from 25 units (`Approach`'s `kPassageOpenDepth`) never opened. A ray ahead of the
  player (`blockedAhead`) widens the reach to 80.
- **A body on the non-collidable layer (15) can still hold the player.** A house's shell behind a Riften
  door (000430A6's Bee and Barb, 042278's Haelga's Bunkhouse, ref 0003C5A1 / 0004FA54, a fixed body under a
  `BSMultiBoundNode` root) kept the player 25 units short of it after `SetCollisionLayer(kNonCollidable)`: the
  body's layer read 15 (ray hit, `hkpCollidable::GetCollisionLayer`), the player still stopped and slid along it. Taking
  the body out of the Havok world (`Doors::Open` with `passable`, `RemoveFromCurrentWorld`) let them through. Why
  the layer change worked for Whiterun's houses and not for these is not known. Riften's doors are recessed in a plank facade whose house shell stands
  ~25-35 units behind the door's centre, so the player stops at depth -5..+10, short of the merged crossing's 15.
- **NPCs shove a standing player.** An NPC (and its dog) walking through a doorway the player stands
  in moves them along its path, 17 units in under a second, with `PlayerCharacter::IsMoving()` false
  throughout (Whiterun, House of Clan Battle-Born's door: Lars and his puppy walking home just after
  the player came out of it). A walk of the player's own reads `IsMoving()` true, so the merged
  crossing (`Approach`) asks for it: a position past the threshold alone is no walk in.
- **Line of sight:** ID 37761 (observer, target, mode), called by detection and combat (IDs 39639,
  39640, 41246, among twelve callers). ID 53829 (`HasLineOfSight`) only dispatches to it.
- **Conditions:** `GetInWorldspace` is ID 21475 and compares exactly. Conditions run through the
  script command table (ID 29924 + 0x691 calls the entry), so replacing the entry answers every
  condition.
- **Teleports** onto a point below the land are lifted onto it (ID 37507 + 0xCEA).

- **Water:** `TESWaterSystem` (ID 400450) lists water objects (+0x20, each: +0x20 shape, +0x40
  reflection) and reflections (+0x38, each: +0x14 plane, +0x40 the material of the water it was
  made for, a plain pointer). A water joining a reflection reads that material (`AddWater`, ID
  32179 +0x697). Water added while the player is indoors (the street's LOD water, the street's
  own) joins the room's reflection and keeps it after the room's water is gone and its material
  freed (`pcb` frees it at once). `RemoveWater` (ID 32182) does nothing while the system is
  disabled (+0xB0). Bridge `water` lists both.

## Rendering a second view

- **Frame:** `Main::RenderPlayerView` (ID 36559) → shadow maps → `RenderDepth` (107139) →
  `RenderShadowmasks` (107140) → `RenderWorld` (107142, executes the accumulated draw lists) →
  `RenderFirstPersonView` (107129) → `RenderWaterEffects` (36560) → post.
- **The main view's depth prepass** (`RenderDepth`) draws most statics with the Utility shader's
  shadow map technique (vertex descriptor 0xC000, RenderShadowmap | Clamped), skinned ones with
  RenderDepth (0x2004). The technique doesn't tell a shadow map from the main view; the depth
  target does (`RendererShadowState::depthStencil`, `kSHADOWMAPS` and the like).
- **Camera data in the frame:** at the start of `RenderPlayerView` the NiCamera has this frame's
  transform but the renderer's camera data (`RendererShadowState::cameraData`) is still the last
  frame's; by `Main::RenderWorld` both are this frame's.
- **The main camera** is the world-root camera (ID 403513, camera at +0x128). `RenderPlayerView`
  pushes it into every light scene's culling process (ID 415016 count, ID 415022 array) and calls
  ID 107132 (camera).
- **Second views in the engine:** the local map (ID 16335 on), menu and inventory 3D (ID 52727),
  the cubemap camera (ID 107564 on), water reflections (the world a second time, with sky and LOD).
- **The menu-3D recipe** (ID 52727), which the portal pass uses:
  1. Culling context {accumulator, camera, scene}, then `BSCullingProcess::Process`.
  2. Set the render and depth targets: IDs 77453 / 77454, with the RenderTargetManager object
     `&ID 411451` as `this`.
  3. `Renderer::SubmitAccumulator(camera, accumulator, flags)`.
  - The accumulator (`BSShaderAccumulator::Create(0x63)`) must have an `activeShadowSceneNode`,
    or the lighting shader's `GetRenderPasses` crashes while culling.
  - It must be emptied after each submit, as `RenderPlayerView` does (ID 106597
    `ClearItemsList`, ID 106600 (acc, 6) and (acc, 5)), or stale passes crash the batch renderer.
  - ID 77386 (false) afterwards marks the engine's state cache dirty.
  - Render flags (`BSShaderAccumulator::RENDER_FLAG`): 0x1 jitters the projection like the main
    view's (`State::SetCameraData`, ID 77503: the cache entry is per camera and jitter); 0x400 calls
    `Renderer::UpdateViewPort(0, 0, true)` (ID 77240), the whole target, without dynamic
    resolution. Binding a target with "update viewport" (ID 77453) applies the render scale
    (`BSGraphics::State` ratios), as for the main view, which is drawn into the target's top-left
    part. The main view submits with 0x41.
- **The far-door clip is not for captures.** The portal pass's clip at the far door
  (`Native::SetPortalClip`, the mask's geometry shader with `SV_ClipDistance`) drew the lighting
  shader's passes flat grey (0.5, depth written) in a doorway picture's capture (3840x2160
  R11G11B10 target, no jitter, flag 0x400), for every shape whose bounding sphere the plane
  crosses: all of a room's walls, floor and ceiling within a couple of hundred units of the door
  (the live pass, jittered at render scale, with the same clip is fine). The picture had colour
  only for what stood further in, so a door seen at an angle (rays through the doorway land on
  the walls and floor beside it) showed grey and white. With the clip off, the capture camera's
  near plane (door plane + 12) does the cut and the picture is whole. Why the geometry-shader draw
  breaks only there wasn't found. To see such a picture: the cache's `.ssdw` is a 72-byte header,
  the BC6H mip chain (mode 11 only, one subset), then half-float distances; flat 0.5 texels with a
  valid distance are the symptom.
- **Scissor:** `RendererShadowState::rasterStateScissorMode` (+0xA4) = 1 with
  `DIRTY_RASTER_CULL_MODE` (0x20) makes the state apply pick rasterizer states with the scissor on;
  the rectangle is whatever `RSSetScissorRects` last set on the immediate context (nothing in
  `SubmitAccumulator` sets one).
- **A culling camera narrows what is drawn, not where:** a shape the doorway-narrowed cull keeps is
  shaded across the whole target. At Solitude's gate the portal pass shaded 24 times the doorway's
  pixels (2.4 of its 3.5 ms GPU) until scissored to the doorway.
- **Draw submission costs ~1.4 µs per render pass** on the main thread (engine state, Community
  Shaders' hooks, D3D11), on the one immediate context: it can't move to another thread.
- **The sun's shadow maps** (`ShadowSceneNode::sunShadowDirLight`, a `BSShadowDirectionalLight`;
  every scene made with ID 106320 has one, sharing the scene's sun `NiDirectionalLight`):
  1. `UpdateCamera(camera)` (vtable +0x80, ID 108496) fits the cascades to the camera (ID 107132
     passes the world camera): split between the camera frustum's near and min(shadow distance,
     its far) by the practical split scheme (linear and logarithmic blended), so a near plane
     pushed out moves every split.
  2. `DirShadowLightCulling` (ID 108492, SE 101498; Community Shaders detours it for Interior Sun)
     culls job arrays (one `BSTArray` of top nodes per culling job, one job per entry of
     `fullFrustumCullingProcessArray`; the world's come from ID 106323) into the cascades'
     accumulators, then a last node list.
  3. `Accumulate(count, 0, nullptr)` (vtable +0x48; ID 107137).
  4. `Render(index)` (vtable +0x50; ID 107133) draws each cascade (ID 107604: binds the depth target
     slice, submits the cascade's accumulator with its own camera) into
     `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` when ID 391108 is 2 (it is in this game), then into
     `kSHADOWMAPS_ESRAM`, and writes the cascades' `lightTransform`s.
  Community Shaders builds its soft copy (Volumetric Shadows' VSM) from the shadow map bound at
  PS t4 when the main view's shadow mask is drawn, and reads `lightTransform` for t98.
  - So each cascade is drawn twice: 2 cascades at 2048 were ~27M pixels shaded, 0.8-0.9 ms GPU, for
    the portal pass. Its maps drawn at half size came out wrong (the ground beyond Solitude's gate
    all in shadow): the cascades are drawn for the engine's resolution. Fitted to the doorway's
    narrowed camera instead of the portal camera, the shadows cost 0.3 ms less main thread but
    0.25 ms more GPU (more pixels per caster).
- **Vanilla specular has no N·L:** `pow(N·H, shininess)` times the light, so a surface lit at a
  grazing angle (a low sun on tree needles) glints at full strength. In the world the shadow maps
  hide most of it; drawn without them, it shows.
- **Rain and snow** are one particle shape per weather (`Sky::precip->currentPrecip` /
  `lastPrecip`, under the world scene's "Weather" node), a cube that follows the camera.
  - The precipitation update (ID 26179, SE 25638; from `Sky::Update` and ID 26222) gives the
    emitter (`BSParticleShaderCubeEmitter`, the shader property's `particleEmitter`) density 0 and
    culls the shape unless `Sky::mode` is full: indoors nothing falls, shown or not.
  - The occlusion map (ID 26175, from `RenderPlayerView` after the shadow masks) is drawn every
    frame indoors too, from the main view's cull roots, as long as the weather has rain or snow.
    ID 26176 switches it (and clears it once).
  - Rain writes no depth: a picture put over the main view later covers the rain in front of it.
  - The shape's `worldBound` is not where the particles are: it is that of the static vertices (Belethor's:
    centre 1275 units off the camera on each axis, radius 705, while the shape's origin is the camera and
    the shader puts every particle in a cube of the emitter's `cubeSize` (850) around it). Anything that
    classifies the shape by its bound (the doorway mask's whole / left out / cut decision) must use the
    camera and the cube instead; by the bound, seen from indoors the street's rain was wholly beyond the
    doorway's planes and left out.
  - The weather's rain and snow are drawn by `BSParticleShader` (its `ENVCUBE` techniques), not the
    effect shader: a hook on the effect shader's SetupGeometry never sees them. Hooked like the others
    (`NativeMask.cpp`, vfuncs 6 and 7 of `VTABLE_BSParticleShader[0]`), the doorway's clip distances
    apply to them. The update (ID 26179, SE 25638) is redirected by moving its first 11 bytes
    (`mov rax, rsp; push rbx; sub rsp, imm32`), as it is called from `Sky::Update` and ID 26222.
- **The main depth as a texture:** `DepthStencilData::depthSRV` of `kMAIN` reads the scene's depth
  after `RenderWorld` (the doorway quad tests against the same view); bind the colour target without
  a depth view while reading it. The depth is the raw value of the jittered `viewProjMat`
  (camera-relative row vectors), so `mul(float4(ndc, depth, 1), inverse)` gives the camera-relative
  position whichever way the depth runs.
- **Two-sided at runtime:** setting `kTwoSided` on a shape's `BSShaderProperty` takes effect at its
  next draw. Community Shaders' lighting shader turns the normals of faces seen from behind (fading
  out past ~10000 units), so they are lit as their own side. Load doors' leaves are often modelled
  from the front only (Markarth's Dwemer doors), since their back faced the other cell.

## Light scenes

- **Our own light scene:** a `ShadowSceneNode` built like the engine's (constructor ID 106320, on
  the engine heap) and registered in `BSShaderManager::State` slot 3 with ID 105635. Slot 0 is
  the world, 1 menu 3D, 2 and 3 are free. It must be registered: shaders look scenes up by
  index (ID 105637, slot → fog property), and an unregistered one crashed the first frame.
- **A cell's lights:** the attach puts them into the world light scene. They are moved into ours
  (`LightScene::Adopt`). On release, call `RemoveAllLights()` on ours, or walking into the cell
  registers every light twice.
- **Lights are culled and faded against the main camera only.** For another view set
  `BSLight::frustrumCull = 0` and `lodDimmer = 1` on each light before the cull.
- **Directional ambient** is one global, set per frame by `Sky::SetDirectionalAmbientColors`
  (ID 105643). Its raw inputs are at ID 414646 (0x48 bytes), the tint at IDs 390997-390999, the
  fresnel at ID 391000.
- **Sun light and fog:** every `ShadowSceneNode` has its own sun light and fog property.
- **No lasting eye adaptation.** The tone map multiplies by `avg.y / avg.x` (ISHDR blend), two
  copies of the frame's average luminance adapted at different speeds: once settled the exposure
  is 1. Interiors and the street are each authored to look right as drawn, so a room seen from a
  sunlit street is as bright as from inside it unless something dims it (`RoomLightScale`).
- **A street in the world scene** while the player is indoors registers one of its lights anew
  every frame. Adopting each into our scene crashed in the world scene's queued removal
  (ID 106335 → 106342).
- **Taking in a light costs ~0.15-0.3 ms.** `AddLight` (ID 106327) only queues it; the scene's
  light update (ID 106335, per queued light ID 106342 → 106401, shadow lights 106400) builds a
  ~200 KB `BSParabolicCullingProcess` for it and walks the scene for the shapes it lights
  (ID 108282, pruned by `worldBound`). It dedupes by `BSLight::light` (the NiLight), not by
  BSLight. Community Shaders still uses the world scene's per-shape lists (without
  them a room came out ~4% darker); our scenes don't need them.
- **Arriving in a cell queues each of its lights twice** in the world scene (Understone Keep: 306
  for 153). The update takes in the first and, finding the second's NiLight active, walks the
  scene for it again. The teleport (ID 40744 + 0x3D2) and every frame (ID 106337 + 0x2B) call the
  update; after a seamless trip `Portal/TransitionStalls.cpp` lets it take 32 lights at a time, nearest first, in
  the world scene and in ours (the space walked out of, attached again with its ~150 lights).
- **The teleport's cost** (ID 40438, loading screen and waits skipped): 15-40 ms into an interior,
  25-60 ms out to a street seen before, 60-70 ms out of Belethor's the first time after a load
  (170 ms before `RemoteCell::Prewarm`).
- **The change into an exterior** (ID 13317) sets the grid up around the arrival cell
  (13288 -> 13428 -> 13430) and calls ID 13427 for *every* grid cell: `TESWorldSpace::LoadCell`
  (from the plugin files if not in memory), the attach if the cell isn't in the grid yet, then
  `GetLand()` and `LoadVertices(true, true)` on its land, which reads (ID 18748) and builds
  (ID 18750) a land not loaded yet and leaves a built one alone. A city worldspace's cells take
  their land from the parent's: `cellLand` is null until `GetLand()` makes it (flags 0x400 with
  the record found, 0x40F once loaded). It also demands the world map's clouds (ID 53142 + 0x272,
  `BSModelDB::Demand` into a global emptied indoors; the path from ID 20495, by default
  `Meshes\Sky\SkyrimWorldMapCloudBank.nif`): ~25 ms from disk.
- **The grid is read ahead for every exterior space we keep** (`RemoteCell::Prewarm`): the cells around the door's cell
  (`uGridsToLoad` / 2) one `LoadCell` (attached and detached again if new) and one `GetLand()->LoadVertices` a frame, from
  the moment the space is ready, kept out of sight (paused) or shown, nearest the door last. Before, only a street merged
  into an interior got this, and only once it was live: the main gate (Tamriel -> WhiterunWorld, `TESFile::SeekCell` under
  IDs 13427 -> 18747 -> 18748 and 20460) took 160-310 ms in the teleport; with it ~100 ms (median of 3, the gate's three
  visits). A prewarm step is under 40 ms (none seen in the hitch watcher). `TESWaterSystem::AddLODWater` (ID 32209, from
  `BGSTerrainChunk::AttachWater` ID 31742 under ID 13317) is ~8 ms of a 70-85 ms exterior arrival.
- **A cell read from the files and never attached** (`LoadCell`, with or without ID 19111's
  temporary references) crashed the AI update (ID 36031 under 35600 -> 35567), also after
  loading a game. Attached and detached again (IDs 18976, 19063, 19130) it stays kUnattached like
  a cell walked past.
- **`ShadowSceneNode::SetPortalGraph` (ID 106385) with a different graph drops every light**
  (`RemoveAllLights`) and both queues. The engine's cell attach (ID 18976 + 0x140) calls it on
  the world scene with the attached cell's graph (`AttachGraphHook` skips it for our attaches).
  It also frees the old graph's list of occlusion shapes (`BSPortalGraph` + 0x10): before the
  skip, every remote attach left the street without its occlusion planes.
- **A switch node's hidden child** (`NiSwitchNode`, a coin bag or mushroom picked or not) is never
  updated: its world transform stays at the origin, and the switch node's bound takes it in.

## Culling

- **A piece of a merged room that loads after the merge is not moved with the room.** The merge sets
  the cell's 3D to the door's place and updates it once. A reference whose 3D finishes loading
  later (inferred, not caught live: a preloaded interior counts as ready at once, its queues still
  empty) is hung under the
  moved cell with the world transform the loader gave it, the cell's own coordinates, and a static
  piece is never updated again: it stands where the room used to be, so the doorway shows the
  street through a room with holes and floating pieces (Belethor's, the Bannered Mare, the Hall of
  the Dead and the Temple in the Whiterun crawl, 20-85% of a room's pieces; each had logged
  `merge failed: door ... has no 3D` from the merge waiting for the room's own door).
  `RemoteCell::SettleLatePieces` finds them (world not parent's world * local) and updates the cell
  again; a loose item of the room (a pot, a coin bag) whose body came late is made visual only as
  `Merge` does, or it is drawn from its body at the cell's own place. Bridge `late_pieces {outOfPlace, settle}` puts a share of a merged room's pieces in that
  state and switches the settling off, for a before/after at one spot.
- **A room merged into the street loses its walls and roof to the street's occlusion boxes, whatever the
  cull mode.** Seen through the doorway in sunlit Whiterun (Belethor's, the Bannered Mare), the big
  pieces of the room (walls, roof planks, counters) were missing and the street's sky and mountains
  showed through, while small things (pots, a fire) were drawn. The world scene's occluder list
  (`BSPortalGraph` + 0x10, 44 shapes in the market) is the cause: with the list emptied the room was
  whole, and the per-object cull told to ignore them (`MergedCull::ProcessHook` with
  `kIgnoreMultiBounds`, tried with all-pass too) changed nothing, nor did emptying the list only
  inside the player view (`BeforeView`): it has to be out from the manager's update on, so something
  before the view reads it. `MergedCull::Set` takes out the occluders within 1500 units of the doorway
  (their bound against the door) until `AfterView`; the rest keep culling the street. The view costs
  ~2 ms more there, partly the room's pieces now being drawn.
  The list is `NiTPointerList<BSOcclusionShape>`: raw shape pointers (the shapes are kept by their cells' 3D,
  refcount 2 with ours), and a node the engine drops goes back to the list's pool with its shape cleared (inferred from
  the null shape, not seen). The
  engine runs between `Set` (the manager's update) and `AfterView` and does drop and add nodes there (cells of the
  grid loading or unloading, the scene taking another graph). Taking every node out and writing them all back
  put a node the engine had dropped into the list again, empty: the next take read its null shape (crash in
  `MergedCull::Set` in the Karthwasten crawl, Enmon's House merged into the street; not reproduced since, so the
  cause is inferred from the crash site).
  Only the nodes taken out are written back now, each after the node that stood ahead of it if that is still
  linked, and not at all when the scene's graph is another one by then or the shape's cell has let it go.
- **An auto-load door is five culled `EditorMarker` shapes** (`AutoLoadMarker01.nif`: Whiterun's
  000351EC, the Underforge's 0006767E): culled by the engine, no opener hid them.

- **A reference's 3D is a copy of a loaded one of the same model, as it is at that moment.** A
  shape hidden with `SetAppCulled` in the first instance of a model (loading a save inside The
  Drunken Huntsman: its own two doors) comes hidden in every instance loaded after (every Whiterun
  dragon door of the street attached next: leaves, handles and backing), through
  `NiObject::CreateClone` (vfunc 0x17). A collision layer too: `bhkRigidBody`'s CreateClone (ID
  78068) builds the copy's Havok body from the original's cinfo (Warmaiden's inside door came
  non-collidable, layer 15, while the inn's door was held walkable). The collision object's flags
  too: Warmaiden's inner doors, loaded while the inn's (same model) were stilled for its merge
  (`kActive` and `kSyncOnUpdate` off), came stilled, their bodies left where the inn's doors stand,
  and couldn't be used. A leaf's turn and a shape drawn two-sided would come the same way (not
  seen). What the portals change in a door's 3D goes
  through `Portal::Pristine`, which makes copies as before the change.
- **The main view's cull** (ID 36643) doesn't walk the scene. It hands each loaded cell's 3D to
  jobs slot by slot, leaving out:
  - slots 0, 1 and 4;
  - the first child of slot 6, an interior's "Shared Portal Geometry", culled through the portal
    graph only.

  `NiNode` and `BSMultiBoundNode` OnVisible aren't called for those nodes. `BSMultiBoundRoom`'s
  (ID 75886) is.
- **Rooms and shared nodes:** a `BSMultiBoundRoom` (ID 75886) or `BSPortalSharedNode` (ID 76448)
  culls its children only when the process's portal graph lists it or the cull mode is kAllPass /
  kIgnoreMultiBounds.
- **A portal's shared geometry:** `BSPortal::portalSharedNode`; the portals are in
  `BSMultiBoundRoom::portalList`, the rooms in the cell's `loadedData->portalGraph->rooms`.
- **A portal's rectangle:** `BSOcclusionPlane::corners` is all zeros in a loaded cell. The
  rectangle is `translation + rotation * (±size.x, 0, ±size.y)`, in the cell's own coordinates
  (Understone Keep's and Mistveil Keep's portals; size taken as half extents, which is at worst
  too big). `entranceRoom` / `exitRoom` are the rooms on either side; rooms have no names.
- **A room's `worldBound`** can be huge (Understone Keep: radius ~93000, centred half way to the
  origin); its multibound (`QPointWithin`) is the one to test points with.
- **Objects outside the rooms** (`BSPortalGraph::alwaysRenderChildren`) hang nowhere in the scene.
  The scene's cull with the graph (ID 106322) culls them one by one.
  - 3D finished after the cell's attach waits in `pendingAlwaysRenderAdds`. It is taken in only
    while the graph is the scene's, so in a cell attached behind a door it never is: most of
    Riften's jail, Mistveil Keep's dais steps.
- **In an interior with a portal graph,** the main view tests what lies outside the rooms against
  the view through the rooms' portals (the process's compound frustum; kIgnoreMultiBounds skips
  that test). A street hung outside the rooms isn't drawn.
  - Listed as always-render with the graph, it is drawn, but then it is also in the sun's shadow
    map, which crashed (EngineFixes' alpha test fix, a pass without a shader).
  - Hung under a room, it is culled with that room's view.
- **Dead ends found while looking for the interior's cull:**
  - The ID 415022 loop isn't a cull: ID 76557 only clears each process's collected geometry.
  - The cull in `RenderDepth` (ID 107139 + 0x710) is the player's skeleton.
  - `ShadowSceneNode` vfunc 0x34 (OnVisible) is never called for the world scene indoors.
- **Occlusion:** the per-object cull (ID 76553, a culling process's vtable slot 0x16) tests an
  object against the process's compound frustum (ID 76821), which holds the occlusion planes and
  boxes of the world scene's graph, unless the cull mode is kAllPass or kIgnoreMultiBounds; then
  only against the view (ID 76564). A room merged behind a street's cliff was hidden by them
  through its doorway (`MergedCull` culls merged cells ignoring multibounds).
- **Tree LOD in an interior's room:** the street's distant tree LOD (`BSMultiStreamInstanceTriShape`,
  shader type 9) hung in a room of the interior came into the sun's shadow map (Interior Sun)
  twice now and then: that batch's pass list then led from its last pass back to its first, and
  `BSBatchRenderer::RenderBatches` (ID 107642, from ID 107133) drew it forever. Its shapes never
  pass through a process's AppendVirtual; keeping the LOD out of the shadow maps' culls, or
  `kCastShadows` off on their shader properties, didn't stop it.
- **The main view's culling process** is a `BSGeometryListCullingProcess` (vtable ID 196460, the
  process in ID 415022), not a `BSCullingProcess` (vtable ID 238422): a hook on one vtable misses
  the other.
- **The doorway mask's plane** lies `Render::Doorway::kMargin` (5 units) beyond the door's
  middle, so the two spaces' frames don't z-fight. A panel shut on the plane is drawn by
  neither space: the world's shut door was cut away (the sky showed), the merged space's door
  near shut vanished. Shut, the mask is sent from behind the panel; nearly shut with the merged
  space's door swinging, from in front of it (`UpdateMerged`).
- **What closes a doorway** (a panel behind the door hiding the void: Belethor's, from a mod; the
  cap inside Solitude's Hall of the Dead's arch, `SMdDEntSol01.nif`) can be one shape with the
  frame and reveals round it, so culling shapes takes the frame too. Nothing culls them: beyond the
  door plane and seen through the doorway, the doorway mask already leaves them out per pixel.
- **`BSCullingProcess::AppendVirtual`** (vtable slot 0x18) is hooked by SkyrimPrecombines (Faster
  Objects); the original is ID 76556.

## The outdoors seen from indoors

- **Distant LOD survives indoors:** Tamriel's terrain manager stays active with its LOD loaded.
  Its land, object and tree nodes are only culled (`SetAppCulled`).
- **The sky stays in the world scene**, but drawn only in an interior that shows the sky: there
  `Sky::mode` is 2 (dome only; the Bannered Mare), elsewhere 1 (interior, the sky's nodes culled;
  Understone Keep in the base game). `Sky::SetMode` (ID 26222, SE 25679; `(Sky*, mode)`) sets the
  mode and culls or shows the nodes for it; the engine calls it on entering a cell. A mod can give
  an interior the sky (Understone Keep had it in the AE test load order).
- **The street can stay attached** when the player goes in (the grid's nine cells).
- **Animated references are run only in the player's cells.** TES (ID 13325) does it each frame
  for the interior the player is in, or else for the grid's cells. A cell's animated references
  are `LOADED_CELL_DATA` +0x40 (added on 3D load by ID 19419 for references whose 3D has the BSX
  "animated" bit, removed by ID 19420), filled in any attached cell. Per frame the engine runs
  their animation graphs (ID 18900, `UpdateAnimation`) and, through ID 18891 / 18875, ID 18894:
  the fade check, a visible-room check, then `UpdateSelectedDownwardPass` of the 3D with the
  frame's time (ID 400333) and flags 2, which runs the controllers and so the particle systems.
  ID 18894 skips the references of a room merged into the street; the selective update on its
  own runs them (`Spaces/Attach.cpp`, `AnimateReferences`).
- **`ShadowSceneNode::particleNodes`** (+0x1C8) is only a queue: the light update (ID 106335)
  pairs each new particle node with lights and empties it every frame. It doesn't animate.
- **`BGSTerrainManager` layout:**
  - `updateNodes` are the level-32 nodes.
  - A node's `children` (+0x30) points to its 4 children inline, 0x50 apart, not to pointers.
  - The low 12 bits of `nodeState` hold one nibble per layer: terrain 0x00F, objects 0x0F0,
    trees 0xF00. The values are 2 none, 4 shown, 8 split into the children.
  - Object layers live on the level 16, 8 and 4 nodes, so a level-32 node without one is normal.

## Doorway pictures: compression and capture from a space out of sight

- **BC6H on the GPU:** the colour is compressed by a compute shader (`Pictures/Gpu.cpp`, `kCompressCode`; BC6H mode 11,
  `Core/Pictures/Bc6h.cpp` is the same maths) into a `R32G32B32A32_UINT` texture one texel a block, then
  `CopySubresourceRegion` with a box in blocks into the BC6H texture's mip (the sizes are whole blocks: `SizeFor` and
  `SizeOf` round up). Checked on WARP: the hardware decode of the blocks agrees with the source (mean error ~1%), and the
  GPU's blocks equal the Core encoder's byte for byte. The distance behind the doorway is a separate `R16_FLOAT` texture
  written by the capture pass's second render target; the march `Load`s it, a BC texture would blur a wall into a chair.
- **A pass for a space nobody looks at:** `PortalPass::DrawAhead` draws a kept (or briefly attached) space with the picture's
  capture camera into the hidden target set. It registers that space's light scene in slot 3 for the draw and puts the shown
  one back, swaps in an accumulator of its own (a new generation made a fresh one and kept the old for good), and passes the
  space's own cells (actors hidden for the capture). `LightOcclusion::SetWorld` is set to that space's world for the pass.
- **Nothing runs in a space attached for a picture:** `RemoteCell::AttachForPicture` skips `MoveToHigh` (the high process is what
  runs an NPC's AI), never steps the Havok world, skips `AnimateReferences` and `CheckLooseItems`. A kept (T3) space is dormant the same way
  until it is live (see "A space's people follow its tier").

## Frame hitches: what held the main thread (measured with the hitch watcher, docs/BUILD.md)

- **A CPU read of this frame's result waits for the whole render queue.** `Map(READ)` on a staging copy of
  the back buffer (the old flash guard, every frame for 60 frames after each crossing) blocked the main
  thread 60-150 ms a frame on this GPU-bound game (99% 3D engine use, the world draw alone 28 ms in
  Whiterun's main street with `perf` on): 841 of the 945 frames over 40 ms in a full Whiterun crawl, in the
  crossing stages. The Present hook's work must never wait for the GPU; decide on the GPU
  (`Render/FlashGuard`: copy, one thread group averages three rows and decides, a fullscreen triangle writes
  the frame or the one before it; the tally is read three frames later with `D3D11_MAP_FLAG_DO_NOT_WAIT`).
- **A frame all in `Present` is the GPU.** With the render queue full the main thread sits in
  `SSEDisplayTweaks < PresentThunk < CommunityShaders < gameoverlayrenderer64 < d3d11 < nvwgf2umx`; the same
  wait shows inside `ID 107644` (`SetupAndDrawPass`) when the driver's command buffer is full. 40-100 ms
  frames of this kind in front of a door looking down Whiterun's street are the scene's own GPU cost.
- **`ID 30451` waits for the task queue to drain.** `TES::SetCurrentCell` (ID 13316 + 0x7E5, AE 1.6.1170) and
  the change into an exterior (ID 13317 + 0x781) end with a call to it on the global ID 400442 unless a flag
  in ID 403330 + 0x340 says otherwise: under a lock it sleeps 1 ms at a time (ID 91402 on a task's state)
  until every queued task has finished. At a seamless crossing with loads still queued (the first door of a
  session: 495 ms, Arcadia's Cauldron 78 ms) it holds the main thread; a loading screen is what it is meant to
  sit behind. `TransitionStalls::QueueWaitHook` skips it on our teleports, like the wait at ID 40438 + 0x8B2.
- **`D3DCompile` takes 10-90 ms.** The doorway mask's geometry shaders are compiled when a draw first needs
  one (`NativeMask.cpp`, one per output signature of the engine's vertex shaders: ~20 of the 35 shaders the
  plugin compiles) and the picture, composite and daylight shaders when first used: a 60-90 ms frame the
  first time a door was opened. `Render::D3D::Compile` keeps the bytecode in `SeamlessSkyrim_Shaders` (named
  by a hash of source, entry, profile and flags); from the second session on it is a file read.
- **A screenshot is a hitch of its own**: `Capture` encodes in the Present hook (50-100 ms and more). The
  crawler's per-stage `frame_stats` covered the last 120 frames, the screenshot's among them, and made
  ordinary doors look like 200-400 ms ones.
