# Changelog

## Unreleased

- Walking out of Belethor's (Whiterun) no longer drops the player through the porch to the land under
  the street (about a third of the launches, the upstream plugin too): on a seamless crossing the
  engine's settle step after the arrival is left out (AE 1.6.1170).
- The patched Community Shaders is gone: the mod draws with the game's own shaders only (also
  under ENB), and no release ships Community Shaders files. Rooms behind doors are masked and lit
  by the plugin itself, one room at a time. Gone with the fork until rebuilt: rooms merged beside
  the shown one, the other worldspace's sun shadows through a city gate, the street's sun shadows
  inside an interior, rain and snow seen through the doorway, the street's light and fog on a room
  seen from across the street, Sky Sync's sun direction, skylighting settling on walking out.
  `[Render] sBackend` is gone.
- Doorway picture files of an older version are no longer read; they are taken again.

- Settings changed in the in-game menu are kept in `SeamlessSkyrim_User.ini` and survive updates;
  `SeamlessSkyrim.ini` is no longer written. A key in the user file wins over the shipped one.
- The old `iWarmPortals` key is no longer read (`iShownRooms` replaced it); `bWorldDoors` and
  `iMergedCullStreet` are gone.
- The experimental `bWalkIntoRooms` option (walk into a room while staying outdoors) is removed.

## 0.6.0

### Doors

- **NPCs cross an open doorway with their walk animation at their own speed, and continue to
  their destination instead of stopping at the door.**
- Doors swing with their own `Open`/`Close` animations; the scripted bounce is gone.
- Exterior doors seen from inside stay shut until the exterior is loaded, then swing open on
  the live view. The captured picture of the exterior is removed.

### City gates

- **Until it loads, a gate shows the current worldspace's LOD city instead of a captured picture.
  Fixes the brick wall in Solitude's gate.**
- Shadows seen through a gate match the city's own at any distance.
- The worldspace behind a gate loads from 4000 units (was 750), and a gate is prioritised over
  the doors around it. A house door still wins when walked up to.
- A gate pair's view is scaled by the ratio of the two doorways, and only when both sides are
  the same mesh more than 1% apart. Whiterun's gate is placed 10% larger on the city side; its
  far arch no longer sits offset.
- A gate's leaves swing in the same direction as the far gate's seen through it, and finish
  swinging before the gate is released.

### Performance

- Persistent references' 3D loads on the engine's model loader thread instead of the main
  thread: attaching at Whiterun's gate takes 21 ms instead of 79.
- After a crossing, light scenes take in 32 lights per frame for 30 frames: the frame after
  leaving Understone Keep takes 30 ms instead of 96.
- The exterior's cells and land are read while the player is still inside: no 200 ms hitch on
  the first walk out after loading a save.
- The portal pass is scissored to the doorway's screen rectangle: 2.35 ms of GPU time instead
  of 3.96 at Solitude's gate.
- A merged room's actors are listed once instead of every frame; the list grew without bound.

### Fixes

- **Items dropped in a room no longer vanish after walking out and back in.**
- A door's 3D is cloned from a loaded door of the same model, and inherited what the portals
  had changed on it. Clones are now made from the unmodified state:
  - doors keep leaves, handles and collision after loading a save inside a building with open
    doors;
  - interior doors loaded while a building with the same doors was merged into the street no
    longer have their collision misplaced.
- Whiterun's gate: the capture built a plain door's shape instead of the gate's, so it re-ran
  every other frame and the view lagged a frame.
- Whiterun's gate: guards fell through the road. Far-side collision is removed from the
  passage only where its bound's centre is on the player's side of the doorway.
- The exterior's image space no longer applies a few steps past a shop door's plane (blue
  tint); it blends in only at the doorway or inside.
- The doorway just crossed shows its space as soon as it loads, without a fade.
- An always-open exterior door is open on the first frame after entering the shop.

## 0.5.0

### Doors

- **Doors open when you use them, without a loading screen, and shut when used again or a few
  seconds after nobody stands in them. They no longer open as you walk up.**
- Doors swing with a bounce off their stop and frame, and play their own sounds.
- NPCs open shut doors on their way through and walk straight through open ones.
- New setting **Doors** (in-game menu, `iForceDoors` in `SeamlessSkyrim.ini`): as set per door,
  every door open (locked ones too), or every door shut until used.
- Walking into an open doorway before the space behind it has loaded takes you through with a
  loading screen instead of holding you at the threshold.
- Door tunings made in the in-game menu are kept in `SeamlessSkyrim_Doors_User.ini` and survive
  updating the mod.

### City gates

- City gates swing open and stand open.
- A gate shows its doorway picture from any distance until the city behind it has loaded,
  instead of the low-detail copy of the city.
- Pictures of gates and double doors are sharper: each is taken in the doorway's own shape.
- Walking back through a gate you just came out of no longer leaves you in the low-detail copy
  of the city.

### Looks

- **Rain and snow fall in the street seen through a doorway or a city gate.**
- A doorway's view is as sharp and smooth as the scene around it with upscaling or TAA, and
  follows the camera when turning.
- The outdoors seen through a door has the sun's shadows: trees behind a city gate no longer
  glint in the low sun where cliffs and mountains shade them.

### Fixes

- The Ratway's doorway no longer shows Riften's canal.
- A door placed at a different size on each side looks like one doorway.
- Riften's arched doors leave no hole in the wall.
