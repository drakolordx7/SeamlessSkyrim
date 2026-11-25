# Plan: doorways that things and sight pass through

Written 2026-11-20, not started. Goal: NPCs see through an open doorway, and arrows, loose objects
and spells pass through it.

The plan comes from reading the docs and the portal code. Nothing was run or reversed for it, so
each phase starts with a spike on its one deciding unknown.

## What stands in the way

- **Two worlds:** the room and the street are separate cells in separate Havok worlds with
  unrelated coordinates. Only the picture is joined, through the door mapping (`Doors::Mapping`).
- **A blocked doorway:** the building's shell, the door panels and the box behind the door still
  have collision. `OpenPassage` lifts it for the player only.
- **Frozen clutter:** a merged room's loose objects have their physics sync switched off
  (`stilledClutter`), so nothing thrown into a room could be seen moving.
- **Only actors are carried:** `NpcTravel` carries people, and only at the shown room's door.

## Phase 0: one doorway service

Everything below needs a list of open doorways that worker threads (AI, the Havok job) can read.

- **Contents:** each doorway's rectangle in both spaces, the mapping, both cells and both Havok
  worlds, published as an immutable snapshot each frame.
- **Two functions:** "does this segment pass through a doorway's opening" and "map this position,
  direction or velocity to the other side".
- **Hole for objects:** bodies in the doorway's prism and the blockers get a tag in their collision
  filter info, and a filter hook refuses those pairs. `OpenPassage` could later use the same
  mechanism.

## Phase 1: arrows and bolt-type spells

1. **Spike:** move a flying projectile to the other cell and world, with position, rotation and
   velocity mapped, and see whether it survives.
2. **Fallback:** if it doesn't, launch a copy on the other side with `Projectile::Launch` and remove
   the original. That is a workaround: it loses some state.
3. **Pre-tag:** a projectile whose path for the next frame passes through the opening is tagged
   ahead of time, since an arrow covers more than the wall's thickness per frame.
4. **Carry:** at the plane it is carried over and its 3D is hung under the other space, as
   `NpcTravel` does for people, so the mask draws it on the correct side.
5. **Check:** hits, damage attribution and crime with the shooter in another cell.

Most spells are missile projectiles and come with this.

## Phase 2: AI sees through

1. **Spike:** find with the `re_*` tools where detection throws out a target in another cell, and
   where line of sight is cast.
2. **Sight line:** for an observer and a target joined by an open doorway, the target is mapped
   into the observer's space. The line must pass through the opening; then one ray is cast in each
   world.
3. **Positions:** detection's distance and view-angle maths get the mapped position.
4. **Follows from that:** sneak eye, greetings, crime witnesses, the start of combat. Pursuit
   through the door already works (`NpcTravel`).

## Phase 3: loose physics objects

1. **Visible motion:** replace the stilling with a sync that places a moving object's 3D in the
   merged space. This also fixes clutter kicked by a room's own people.
2. **Threshold collision:** copy each side's fixed collision within about 150 units of the doorway
   into the other world (`RemoteCell::MergeCollision` did this for a whole room, removed with
   walk-into-rooms, in git history), so objects have a floor on
   both sides.
3. **Transfer:** active bodies near a doorway are moved to the other world and cell when their
   centre crosses the plane, with velocities mapped.

This is the usual portal method. A body halfway through collides with only one side's movable
objects.

## Phase 4: the rest

- **Beams (lightning):** continue the cast on the other side, drawn as two segments.
- **Flames, cones, shouts, explosions:** volumes, not points. Left out until the rest works.
- **NPC archers and mages:** their aiming needs the mapped target position, on top of phases 1
  and 2.
- **More doors:** rooms beside the shown one, and city gates. Everything above is for the shown
  room first, in both directions.

## Testing

Each phase gets a stored place and route at the Bannered Mare and Belethor's, and bridge commands
worth keeping:

- `fire_projectile`: point, direction and type.
- `detection`: who detects whom, at what level.
- `push_object`.

Arrow crossings are judged with every-frame captures, before and after.

## Order

0, 1, 2, 3. Arrows are the smallest piece and prove the service. Sight is independent of physics
and answers the oldest complaint. Loose objects are the most work for the least visible gain.
Phases 1 and 2 can swap: they share only phase 0.
