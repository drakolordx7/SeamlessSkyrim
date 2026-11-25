# Approaches: what is in use, and what was tried and dropped

Read this before proposing a new approach. The engine facts behind it are in ENGINE.md.

## What is in use

| Situation | How the other side is shown |
|---|---|
| Outdoors, a door into a room | The room merged into the world scene (3), up to four at once (`[Portals] iShownRooms`) |
| Indoors, the door out to the street | The street merged into the interior (9, `[Portals] bMergeOutdoors`, on) |
| Interior ↔ interior, exterior ↔ exterior (city gates) | The portal pass (1); through a city gate the world's own shapes are masked as far as the portal pass draws (`UpdateGateMask`) |
| A room's doorway before its merge | A picture taken with the portal pass, kept per door. From inside a building, none of the street: its door stays shut until the street is shown live (`StreetShownLive`) |

Crossing is the engine's own door teleport without its loading screen (2). A doorway standing
open before the space behind it is there to walk into (a stand-in) is taken the engine's way, with
its loading screen, when the player walks into it (`WalkIntoStandIns`); its door stays solid until
then, or the player walked on into whatever stands behind it in their own space.

A door opens when it is used (the player, or an NPC reaching for it on its way through) and
shuts 5 s after nobody stands in it; city gates and the doors set to stand open always stand
open (`DoorUse`, `Doors::Wanted`). Until the city behind it is there, a gate
shows the player's own worldspace through its gateway: its low-detail copy of the city, in 3D,
under the real sky; its own leaves swing into the side the far gate's do with the portal on
(not where the gates stand apart: there they would swing into the masked passage). Where the two gates stand more than 150 units apart (Solitude's, 370), the
player's worldspace walls the passage up between them; the gate mask (`StandInGatePassage`)
hides its shapes through the gateway up to just past the far gate. A gate set to
(`[DoorFarPicture]`) shows its picture instead. Dropped: the picture for every gate. Taken from
a fixed camera and reprojected with its depth, it never looked like the street behind (the room
door's camera gave the city ~90 texels; one 1000 units back smeared the gatehouse from close by).
A doorway's picture has the doorway's own shape when it is wider than three quarters of its
height.

## Overview

| # | Approach | Status | Where it fell short |
|---|---|---|---|
| 1 | Portal pass: the other space drawn by our own reduced render, put into the doorway | In use where nothing can be merged | A reduced render, never quite the real look |
| 2 | Crossing without a loading screen | In use | (kept in every design) |
| 3 | Render-merge: the room drawn by the game's own main pass, masked at the door | In use, looking in | On its own only one way |
| 4 | Walk into rooms: the game kept outdoors inside the room | Removed (in git history before this change) | Visible switches, too many, NPCs can't interact |
| 5 | Sectors: interiors moved into the worldspace by a generator | Dropped, code removed | Houses don't fit their shells; "where is the player" faked everywhere |
| 6 | Open cities by merge | Dropped, code removed | The edge between the two worlds can't be fixed automatically |
| 7 | Open Cities (the existing mod) as a base | Not taken | A dependency; loses mods' navmesh edits |
| 8 | A second full render of the outside | Not tried | The engine switches the outdoors off while you're inside |
| 9 | The street merged into the interior | In use, looking out | (the current design) |

## 1. Portal pass

The other space is rendered from the main camera carried through the door pair into our own
colour and depth targets, then drawn on a quad in the doorway. It follows the engine's recipe
for menu 3D (ENGINE.md), since the main pipeline didn't accept a second camera. It has its own
light scene with the other cell's lights.

Why it falls short: it isn't the game's world render.
- **No deferred lighting:** the pass runs with `inWorld == false`, so Community Shaders' deferred
  path and its effects don't run.
- **Sun shadows of its own, soft:** an outdoor space's sun shadow light (its light scene's) is run
  through the engine's sequence with the portal camera into shadow maps of ours
  (`RenderPortalShadows`), and the fork lights the pass with their soft copy (Volumetric Shadows'
  VSM, 512 texels a cascade), not the world's crisp shadow mask; terrain and cloud shadows as
  outdoors (the other worldspace's heightmap). Community Shaders fork only.
- **A doorway picture's capture frame** draws the pass with the capture camera into the hidden
  target set, so the doorway shows the previous frame's picture that frame. A picture that never
  came out the size asked for was retaken every other frame for good (Whiterun's gate: the view
  through it stuttered and lagged a turn by a frame half the time).
- **Drawn like the main view:** at its render resolution and with its jitter, so the upscaler
  resolves the doorway with the scene (a doorway picture's capture is drawn at full size).
- **No real sky:** a gradient is drawn instead (`DrawSky`).
- **No distant LOD and no fog.**
- **Rain and snow:** through a city gate, the main view's own rain is drawn into the picture
  with the main camera (`DrawMainPrecipitation`): the picture covers it on screen. Rain drawn from
  the portal camera was only the far, thin streaks (its near plane stands at the far gate).
- **Workarounds needed:** freed render passes (a crash, now caught), motion vectors for the
  upscaler, and borrowed engine state that has to be saved and put back.

Each missing feature would need its own re-implementation, and it would never quite match.

## 2. Crossing without a loading screen

The engine's own door teleport, with the loading screen, its waits and the autosave skipped. The
street is handed over to the engine still attached. About 45 ms per crossing.

## 3. Render-merge (looking in)

The room's 3D is hung into the world scene, moved by the door mapping, and the game's main pass
draws it at full quality. The render backend (RENDER-BACKENDS.md) masks the two spaces per pixel
at the door plane and lights the room's shapes with the room's own light.

Before the main view, `MergedCull` hides what the mask would throw away whole: everything when no
line of sight passes the doorway, otherwise the shapes outside the space seen through it. An
interior built of rooms is also culled through its portals, from the doorway or from the room the
camera stands in: rooms not seen through a chain of portals aren't drawn, and the others are
tested against what is seen of them (Understone Keep from Markarth's street: frame 35 to 19 ms).

On its own it only works one way: the outdoors' terrain, LOD, sky and water are global systems
and can't be moved into the interior. What made the other way possible is 9: the interior's
scene takes the street in, and the outdoors' global systems turn out to survive indoors.

## 4. Walk into rooms (`[Portals] bWalkIntoRooms`, removed: in git history before this change)

The game keeps the player outdoors while they walk into the merged room:
- the room's collision is copied into the outdoor physics world;
- the view out is the game's own outdoors, at full quality;
- the game switches into the interior only once the doorway has been out of view for 3 s, and
  back out when the player heads for the door.

Why it was removed:
- **Every switch showed:** a long frame, the light stepping, the player's own head for a frame.
- **Far too many switches:** 58 in seven minutes of play, once a second on the stairs of the Hall
  of the Dead.
- **The room's people can't see, greet or fight the player** until the switch, because the player
  is in another cell.

Last tested: with the camera inside the room, the room's walls and floor weren't drawn (its
furniture was), and the outdoors showed through. Not investigated.

## 5. Sectors: interiors moved into the worldspace (dropped)

A generator (`tools/places`) moved an interior's references, navmesh and lights into the exterior
at its door. At runtime a collision filter kept the two spaces apart, per-pixel masking hid each
from the other, and a "light box" gave the inside the interior's lighting. Proven on Swindler's
Den and the Bannered Mare: people walked out of the inn onto the street, seams between rooms
worked, and line of sight worked across a seam.

Why it was dropped:
- **Houses don't fit their shells.** Rooms are larger than the building around them. Of 154
  single-cell interiors with two or more outdoor doors, 4 fit within 64 units at every door
  (Whiterun's shops are 290-410 units off). Places had to be cut into pieces joined by seams.
- **"Where is the player" is wrong everywhere.** The player's cell is the street, not the inn:
  quest conditions, location, the interior flag, ownership and trespassing, and music all need a
  faked answer. Every query that gets missed is a silent bug.
- **Size:** every house of a city puts its references and navmesh into the outdoor cells.

Worth knowing if it comes back (the code is in git history before its removal):
- **Moved references:** persistent ones keep their FormIDs, so quests and aliases still point at
  them. Temporary ones were copied, with the originals disabled.
- **Rotation:** a placed reference's rotation is `Rx(-x) Ry(-y) Rz(-z)`. A turned place needs
  the full composition; adding yaw to z left tilted rocks floating.
- **Interior occlusion data** (room and portal markers, multibounds, room links) crashed the
  engine in an exterior cell (ID 19165).
- **Navmesh:**
  - Each piece has to become more triangles of its cell's outdoor navmesh. Pathing follows links
    between navmeshes only at cell borders and load doors.
  - Every plugin's NAVI lists the navmeshes it adds or changes. A navmesh without an entry
    crashed the load.
  - Cell coordinates are stored Y first, and every navmesh carries the CRC A5E9A03C.
- **Directions into a place** come from the teleport targets, never from door rotations.
- **Floors:** a door's origin is ~50 units above its floor.

## 6. Open cities by merge (dropped)

A generator moved each city worldspace into Tamriel, and a runtime hook answered the
`GetInWorldspace` condition for the city. Whiterun worked end to end: walking through the gate,
people pathing both ways, save and load, quest conditions.

Why it was dropped:
- **The edge between the worlds.** The city worldspace and Tamriel were each built to look right
  only with their own surroundings. Along the walls, the two worlds' rocks, terrain pieces, light
  cards and water planes met: a lot of broken geometry. Open Cities fixed that edge by hand.
- **Compatibility** needed a growing list of runtime answers.
- **FormIDs:** copied references got new ones, which could shift between runs of the patcher.

Worth knowing:
- **Shared coordinates:** city worldspaces share Tamriel's coordinates. WhiterunWorld also holds
  4000+ DynDOLOD copies of Tamriel's land and objects for the view from inside.
- **Twins:** the same piece in both worlds must be kept once, not disabled as a stand-in.
- **Gates** have opening animations (ENGINE.md) and swing open like doors.
- **Pathing:** the city's navmesh must lose its door triangles, or the AI's pathing crashes on a
  gate that leads nowhere.

## 7. Open Cities as a base (not taken)

Build on Arthmoor's hand-made edge and let a patcher only forward other mods' changes onto it.
Not taken because it is a dependency, and mods' edits to a city's navmesh are lost under it.

## 8. A second full render of the outside (not tried)

Run the game's own render a second time for the doorway: the outside, from the camera carried
through the door, into our own targets.
- **Estimated cost:** 3-6 ms per frame, only while a doorway is in view. The doorway sees a
  quarter to a third of a normal view, shadows can cover only that view, and screen effects
  touch only the doorway's pixels.
- **Precedent:** water reflections render the world a second time every frame, with sky and LOD.
- **The blocker isn't cost.** Once the player is inside, the game has switched the outdoors off:
  sky and weather, the sun and its shadows, distant LOD, outdoor lights. Community Shaders also
  takes its per-frame data from a frame's first pass.

## 9. The street merged into the interior (in use)

Render-merge the other way round: the game is in the room, and the street's cells, the distant
LOD and the persistent references are hung into the interior's scene, moved by the door mapping.
The game's own renderer draws the view out at full quality.
- **The outdoors survives indoors** (ENGINE.md): the distant LOD is only culled, the sky stays
  in dome-only mode, and the street can stay attached.
- **Masking:** the doorway mask is sent as for a room.
- **Light:** the street is lit with the outdoor sun and ambient, worked out from the weather the
  way the sky does it (equal to the engine's to 1e-7), times the outdoor image space's sunlight
  scale (without it the street was a dull grey).
- **Sun shadows:** Community Shaders' Interior Sun is switched on (`SeamlessPortal_SetInteriorSun`),
  and the engine's sun shadow light gets a directional light of its own pointed at the outdoor
  sun. Its shadows fall on the street only; the room keeps its own unshadowed directional light.
- **Fog:** the street's own fog.
- **Rain and snow:** the engine's precipitation update runs as outdoors while the street is
  merged (`PrecipitationUpdateHook`); the fork's particle shader keeps the rain to the street's
  side of the doorway and lights it with the street's light. Community Shaders fork only.
- **Doors:** both door panels are hidden. The interior's shapes standing just beyond the door
  plane are culled (Belethor's backing panel).
- **Culling:** the street hangs in the interior's statics branch, or in the door's room for an
  interior built of rooms (Riften's jail).
- **Crossing out** is the portal's, and the street is handed over to the engine attached.

How it was found to work: a RenderDoc capture inside Belethor's. The street's shapes were drawn
in the depth pre-pass and discarded by the shader, because the doorway mask was another door's.
`World::Sectors` (removed since) sent its nearest door every frame, after the portal manager had
sent its own, and Community Shaders then held only one doorway mask.

Still open: people in a room looking out are in another cell than the street's people.
