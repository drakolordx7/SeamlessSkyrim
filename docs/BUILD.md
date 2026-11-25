# Building, deploying and testing

How to build both halves, get them into the game, and drive a test, on this machine. Engine
facts are in ENGINE.md.

## Paths

| What | Where |
|---|---|
| Game (AE 1.7.104) | your Skyrim Special Edition install (`<game>` below) |
| Test instance (what the agent harness launches) | the Skyrim modding project's MO2 portable instance, `D:\AI\Active\Skyrim\instance` (`SKYRIM_MO2_DIR`), profile `SeamlessTest` |
| Plugin log | `Documents\My Games\Skyrim Special Edition\SKSE\SeamlessSkyrim.log` |
| Crash logs | same folder, `Crashlogs\*.log` (trainwreck); `re_lookup` turns a `SkyrimSE.exe+offset` into an ID |
| Plugin settings | `SeamlessSkyrim.ini` (shipped) and `SeamlessSkyrim_User.ini` (what the in-game menu writes, wins per key), next to the DLL in `SKSE\Plugins` |
| Load order | `%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt` (the Steam game; MO2 keeps its own in the profile) |

## SeamlessSkyrim (this repo)

```powershell
tools\build.ps1 -NoDeploy          # compile only; works while the game runs
tools\deploy-mo2-dev.ps1           # put that build into the MO2 test instance
tools\build.ps1                    # RelWithDebInfo, and copy it into SKYRIM_PLUGINS_DIR if one is set
```

The script finds `cmake`, which isn't on `PATH` (it comes with Visual Studio), and stops before
building when the game is running and the copy would fail.

- Output: `build/RelWithDebInfo/SeamlessSkyrim.dll` and `.pdb`.
- **MO2 test instance** (what `game_launch` starts): `tools\deploy-mo2-dev.ps1` writes the DLL and
  PDB into the local mod `Seamless Dev Build` of the Skyrim modding project (both its
  `.weaver\files\<mod>` source and `instance\mods\<mod>`, with the build's modification time so
  that project's `build.py` leaves them alone). The mod sits right after `Seamless Skyrim` in its
  `modlist.yaml`, so it wins over the Nexus release's DLL while that release's Community Shaders
  files, ini files and shaders stay. It refuses to run while `SkyrimSE.exe` runs. `-Baseline`
  deploys `.weaver\baseline` (the upstream plugin) for comparison, `-Source <folder>` any other
  DLL. Once, from the Skyrim project: `uv run .weaver/tools/build.py --only "Seamless Dev Build"`
  (the script says so when the mod doesn't exist yet).
- **A game folder**: configure with `-DSKYRIM_PLUGINS_DIR=<game>\Data\SKSE\Plugins` and the
  post-build step copies both files there; it fails while the game runs. Empty by default (no
  copy).

### Layout and Core

`src/Skyrim/` is the plugin (everything that touches the engine), one folder per domain:

| Folder | What lives there |
|---|---|
| `Plugin/` | `main.cpp` (SKSE entry, the order things are installed in), `Settings` (the one owner of the ini files), `Frame` (the player-update hook that drives the portals) |
| `Engine/` | Engine helpers shared by domains: attach/detach a cell, fade, fog layout, same-space, hook-site checks, `CoreConvert.h` |
| `Doors/` | One door pair: geometry, swing, held-open count, mapping, per-door ini, policy |
| `Spaces/` | `RemoteCell`: the space behind a door kept alive (attach, Havok step, placement into the world scene, a gate's passage, engine hooks), and `WalkOut` |
| `Graph/` | `DoorScan`: the door graph (`Core::Graph::SpaceGraph`) from the placed teleport doors the engine has in memory: a scan at data load, then each cell as it loads. Bridge: `graph_summary`, `portal_status.graph` |
| `Portal/` | `PortalManager` (frame order), `Schedule` (the tier scheduler run on the game: what is kept loaded, which door is live), selection (portal-door tests and ranking), merged view/cull, crossing (driven by `Core::Crossing::Sequence`) and travel, lights, street merge |
| `Render/` | The native renderer (`Native/`: doorway mask, lighting swap), ENB detection, portal pass, space lighting, Present hook |
| `Pictures/` | Doorway pictures: capture, cache, composite |
| `Bridge/`, `Diag/` | Test tooling (agent mode, the TCP bridge, perf, sampler); `Diag/Telemetry.h` is the no-op sink the portal code reports to |
| `UI/` | The in-game menu |

`src/Core/` is the game-independent library `SeamlessCore` (plain-float maths, ids and the
decisions that need no engine types), linked into the DLL. Core has no CommonLib, no PCH and no
Windows or D3D headers: `cmake/CheckCore.cmake` fails the build when a file under `src/Core`
includes anything but the standard library or another Core header. Engine types are converted at
the boundary in `src/Skyrim/Engine/CoreConvert.h`.

The unit tests of Core are a separate executable, not part of the DLL. `tools\build.ps1` builds it
with everything else; run it with `build\RelWithDebInfo\SeamlessCoreTests.exe` (exit code 0 when
all checks pass). The source is `tests/Core/CoreTests.cpp`.

## Door tunings

Doors tuned in game land in `SeamlessSkyrim_Doors_User.ini`, never in the shipped
`SeamlessSkyrim_Doors.ini`; `tools/fold-door-tunings.ps1 -GameData <game>\Data` (run by
`make-release.ps1`) moves them into `package/`'s file and the game's and deletes the user file.
Commit `package/`.

## Release archive

`tools/make-release.ps1 -GameData <game>\Data -Build` writes `build/release/SeamlessSkyrim-<version>.zip` (version from
`CMakeLists.txt`), a FOMOD installer (`package/fomod/`):

- `Core/`, always installed: the plugin (Release, no PDB) and `package/`'s two ini, the in-game
  door tunings folded in first (commit them).
- `LICENSE`, `SOURCE.txt` and `THIRD-PARTY-NOTICES.txt` at the archive's root: the GPL text, the
  GitHub link to this repo (`-SourceUrl`), and the license of every library in the DLL, gathered
  from the build tree's `vcpkg_installed/<triplet>/share/*/copyright` and the license files under
  `extern/`.
- Without `-Build` a DLL older than its sources stops the script.

## Driving the game from an agent (tools/skyrim-mcp)

Launching and the bridge:
- `game_launch(save=...)` starts the game with the agent bridge and loads the save straight from
  the main menu (`SEAMLESS_AGENT_LOAD`). By default it goes through the MO2 instance
  (`ModOrganizer.exe moshortcut://:SKSE`, so the mods are in the game's virtual file system) with
  the profile `SeamlessTest`: its own saves folder, autosaves off, so a test run never touches the
  user's saves. It refuses while MO2 itself is open (a game it starts has no bridge) and for a
  profile without local saves. `SKYRIM_LAUNCH=skse` with `SKYRIM_GAME_DIR` starts the Steam game
  directly instead; every `SKYRIM_*` setting (MO2 dir, profile, base save, docs dir, saves dir) can
  also go in a `harness.json` beside `server.py` (see the top of that file).
- A route's `{"load": name}` save that doesn't exist on this machine falls back to `starts.json`:
  the stored place or cell (and console lines) to start from, applied to the clean new-game save
  `SeamlessTestBase` (`make_base_save`).
- Never launch or quit while the user is playing: check `Get-Process SkyrimSE` first. A game the
  user started has no bridge, and the plugin log says `agent mode: off`.
- Background mode moves the game window to a second virtual desktop **once** per launch and then
  leaves it alone. `game_window_mode("show")` brings it to the front.
- Launched games are silent (`game_audio` toggles it). The plugin mutes XAudio2 inside the game,
  never the Windows volume mixer: Windows keeps a mixer mute across runs, so a crash while muted
  left the user's own games silent.
- Changes to `server.py` only apply after the MCP server is reconnected (`/mcp` in Claude Code).
- A tool returning text and an `Image` in a list needs `@mcp.tool(structured_output=False)`: with
  the `list[Any]` annotation, mcp 1.30 tries to serialize the `Image` and the call fails after
  the work is done ("Unable to serialize unknown type").
- `tools\build.ps1` doesn't run from the Bash tool (execution policy); use the PowerShell tool.

Tests:
- A game loaded at launch and one loaded later in the same session are not the same test (the
  persistent references, ENGINE.md). Loading saves back to back across worldspaces with portals
  on crashes now and then (KNOWN-ISSUES.md): relaunch rather than run a long reload loop.
- Walking through a door: `walk_to` a position a little past the door along its inward direction.
  Aimed at a door reference in the other space, the player crosses at an angle beside the
  doorway, and the engine moved them out of the frame's collision, under Belethor's porch.

Scripts beside the MCP server (for loops, and commands with no MCP tool such as `schedule`):
- The bridge serves several clients at once (127.0.0.1:47800, one JSON request per line).
- Run with `PYTHONPATH=. uv run python script.py` in `tools/skyrim-mcp`, then `import server`:
  `server.bridge.call(cmd, **args)` for a bridge command,
  `getattr(server.run_route, "fn", server.run_route)(steps=[...])` for an MCP tool.
- `re_read` takes an offset from the module base. For a heap address pass `address - base`; the
  base is `(Get-Process SkyrimSE).MainModule.BaseAddress` (0x7FF73F6A0000 on every launch so far).

`press_key` goes through the bridge (`key`): the plugin adds the press to the game's own input
queue in its keyboard read, so it needs no focus and nothing reaches other windows. A key only
does something in a menu where the menu-mode control map names it: `e` accepts a message box,
`esc`/`tab` cancel; Enter isn't mapped there. `os_input=true` falls back to Windows input (focuses
the game). Message boxes from mods missing their DLL (MCM, OStim) pause the game after a load;
`e` once per box.

GPU memory (bridge, from a script): `gpu_live` starts tracking every texture and buffer the
device makes and lists the live ones grouped by the stack that made them (`since`: a mark from an
earlier call, to see what a walk left behind); `gpu_holders` scans memory for what keeps the
largest of them (`orphans: true`: D3D textures whose engine texture is already gone), with the
class of each holder from MSVC RTTI; `mem_holders` does the same for any addresses. Freed memory
still holds old pointers, so a holder is a lead, not proof. Tracking hooks AddRef/Release of
textures: leave it off for timing measurements. `(Get-Counter "\GPU Process Memory(pid_<id>*)\Total
Committed")` shows GPU memory the game holds; private bytes follow it.

Test suite (`run_suite`):
- Runs the routes listed in `tools/skyrim-mcp/suite.json`: only paths that broke more than once,
  not every stored route. Launches the game if needed; a route the game crashed in runs once more,
  the plugin's log of the crashed game kept beside the run's pictures (the relaunch starts it anew).
- A route step `{"check": {"name": ..., "merged": true}}` calls bridge `scene_check`: still things
  (statics, trees, furniture, containers) around the player and around the door in the merged
  space must hang in the scene where their reference puts them, and a merged street's land and
  distant LOD too. It also takes a picture. `"worldspace": "<form ID>"` also fails it when the
  player is elsewhere: walked through a city gate without the crossing, they stand in the street
  worldspace's low-detail copy of the city, and the scene check passes there.
- `walk_to` stops at a door crossing ("travelled through a door"): a route walking on past it
  needs a second walk step.
- The pictures come back beside the last accepted ones (`accept=true` makes this run's the
  accepted ones; kept in `tools/skyrim-mcp/suite-reference/`, not in git). Look at them: the check
  can't see a wrong colour or a missing person.
- From a script: `getattr(server.run_suite, "fn", server.run_suite)()`, as for other MCP tools.
  The MCP tool itself currently fails like `frames_sheet` ("Unable to serialize ... Image"), after
  running every route: use the script.

Every frame of a moment (a flash on crossing, a door drawn closed once, low detail over the real
town falls between screenshots):
- `run_route(name, capture=true)` saves the frames of each crossing and of the merge that follows
  it (40 frames, about a second) and lists them under `bursts`, each with its `oddFrames`: frames
  that differ from the one before and the one after, which resemble each other. A route step
  `{"frames": n}` starts a burst at that point of the route. An NPC carried through the portal's
  door starts an `npc` burst (60 frames).
- NPCs walking through a door: route `bannered-mare-npcs-evening` (Whiterun at 19:48, timescale
  60: three or four walk into the Bannered Mare within 30 s). Writing a burst out stalls the game
  ~2.5 s, which shows in any timing taken across it. Papyrus `Actor.PathToReference` dispatched
  through the VM (`DispatchMethodCall1`) didn't move anyone; the schedules do.
- `capture_frames(frames, crop)` takes a burst now and returns it as one contact sheet. `crop`
  cuts to the doorway and makes the odd-frame scores more sensitive. The single frames stay in
  the burst's folder at full size.
- `capture_frames` can fail with "Unable to serialize ... Image" after the frames are saved;
  `frames_sheet` on the burst's name then shows them (it works now).
- A door's swing: bridge `capture_frames {frames, directory}` returns at once, so call it and
  then `activate {ref}` on the door; the MCP `activate` returns too late for the start of the
  swing. It also leaves the camera turned: `camera_view {firstPerson: true}` and
  `player.setangle` before the next picture. Bridge `doors {force: 0|1|2}` sets the global door
  setting for the session (2: every door shut until used).
- The odd-frame score only finds what lasts a single frame. Something wrong for several frames in
  a row (a weapon drawn black for four) has to be seen.
- Not while measuring stutter: writing a burst out stalls the game for a moment.

Diagnostics:
- `dump_world` writes the outdoors probe to the plugin log: the sky and its colours, the computed
  outdoor lighting, the doorway, and the street references that can't be drawn and why.
- `wide_bounds {cell, radius}`: nodes of a cell's 3D with a bound wider than `radius`, with their
  farthest children (a stray child at the origin makes every bound above it span the map).
- `portal_status` has the active portal, the remote cell, the merged rooms and the distant LOD
  (`lod`: shapes of its land, objects and trees).
- A piece of a merged space missing:
  - bridge `ref_3d {ref}`: the chain of parents with node types and each node's slot in its
    parent, and each node's physics body (flags, motion type, where the physics has it, its
    constraints);
  - `find_nodes {name}`: nodes by name in the world's and the portal's scene;
  - `world_occluders {action?}`: the world scene's occlusion planes and boxes; `"clear"` takes
    them out of its list (to see whether they hide something), `"restore"` puts them back;
  - `orphans {cell?}`: the remote cells' references whose 3D hangs nowhere, and each portal
    graph's always-drawn and pending lists.

Frame rate:
- `perf {"on": true, "reset": true}`, wait, `perf {"on": false}`: per frame over the last 300,
  main-thread ms of the portal's and the manager's parts (`cpu`), GPU ms and thousands of pixels
  shaded of the world draw, the portal pass and its submit (`gpu`), and counts. A
  long `present` means the GPU (or frame generation) holds the main thread back. Off by default:
  its queries cost a little every frame.
- A crossing frame by frame: `perf {"on": true, "reset": true}`, walk through, then
  `perf {"last": 300}`: each frame's scopes (`frame` is the time between presents), the crossing's
  frame marked by the count `crossing`; its parts are `crossing begin` (`release kept`,
  `release shown`), `teleport` (the engine's) and `arrival`, then `attach`, `merge`,
  `lights: world` / `lights: other`. The crossing is carried out after its frame's present, so it
  lands in the frame marked.
- Route `whiterun-gate-attach` (save `SeamlessTestWhiterunGateOutside`): launched with it, the gate's
  attach queues WhiterunWorld's persistent references (log: `queued for loading`, `attach took`);
  loaded again later in the same session there is nothing to queue.
- The `markarth-understone-*` routes' quicksave is gone. Understone Keep from
  `SeamlessTestMarkarthMines`: walk out of the house, `player.moveto 16E47`, then the routes' `move`
  and walk steps. `place_go understone-keep-front` from inside the house only moves the player
  within the house (its cell is `Wilderness`).
- `schedule` returns the scheduler's state (`schedule`, `warm`), frame times (`frames`) and the main thread's time in the player view
  (`mergedCull.viewMs`, culling and draw submission).
- `profile {file, seconds}` samples the main thread now; `travel_profile {file}` for 1.5 s from
  the next crossing. The first second or two after a merge is the light-occlusion rays catching up
  (`lightOcclusion.raysTotal` in `portal_status`).
- `sampler_report.py file [from_ms to_ms]` names the functions in SeamlessSkyrim's frames
  from the build folder's PDB, so the DLL in the game must be that build.
- The camera moving changes the numbers a lot. Measure with nobody at the controls.
- Left alone, the game's idle camera starts orbiting the player after a while (camera state 1 in
  bridge `camera`), so pictures and numbers taken a few seconds apart look in different
  directions. `console setini "fAutoVanityModeDelay:Camera" 100000` and bridge
  `camera_view {firstPerson: true}` before comparing.
- `move_to` across the door plane into the merged room teleports into its cell at the street's
  coordinates (the player falls in the void): load the game again.

## SE 1.5.97

A second game install, `<se>` below (SKSE 2.0.20, Address Library `version-1-5-97-0.bin`
copied from the AE game's plugins folder, trainwreck). It shares
`Documents\My Games\Skyrim Special Edition` with AE: logs, crash logs, ini files and saves.

- Deploy by hand: copy `build/RelWithDebInfo/SeamlessSkyrim.dll` and `.pdb` (and the two ini
  files) into `<se>\Data\SKSE\Plugins`. `SKYRIM_PLUGINS_DIR` copies into one game only.
- The MCP server drives it with `SKYRIM_GAME_DIR=<se>` (set when the server starts;
  from a script: `SKYRIM_GAME_DIR=... PYTHONPATH=. uv run python script.py` in `tools/skyrim-mcp`).
- The saves are AE saves and don't load in 1.5.97 (a crash after loading). Start from the main
  menu with `coc`/`place_go`. A place whose cell is `Wilderness` (`understone-keep-front`) can't
  be reached that way: `coc Wilderness` picks some cell of that name, here one without a
  worldspace, and crashed; use `cow MarkarthWorld -44 0` and `move_to`.
- A new game asks about Survival Mode after a minute and the prompt holds the player. With OS
  input `press_key` didn't reach it in background mode (`game_window_mode("show")`, `right`,
  `enter`, then back); the bridge's keys (`right`, then `e`) not tried on it yet.
- Doors start locked in a new game (the Smelter Overseer's House): a locked door gets no portal.

### AE 1.6.1170

A third game install, `<ae1170>` below (SKSE 2.2.6, `versionlib-1-6-1170-0.bin` from the AE game's
plugins folder, trainwreck). Same shared Documents folder.

- It closes at startup: it can't parse the `ContentCatalog.txt` that 1.7.104 writes into the
  shared `%LOCALAPPDATA%\Skyrim Special Edition` (keys `CSV2_<guid>`). Move that file aside until
  the main menu is up, then put it back.
- Its exe on disk is encrypted too: `dump.py` as for 1.5.97. `addrlib.py` reads its
  `versionlib` (format 2).
- Deploy by hand like 1.5.97, into `<ae1170>\Data\SKSE\Plugins`.

Porting an address (every engine address is a `REL::RelocationID(se, ae)`; call sites inside a
function take `REL::Relocate(seOffset, aeOffset)`, or `Hooks::Offset(se, ae, ae1799)` where AE
before 1.7.99 differs):

- The 1.5.97 exe on disk is encrypted (Steam's DRM); its code is read from the running game:
  `python tools/se-port/dump.py <pid> build/se-port/SkyrimSE-1.5.97-mem.exe` once at the main
  menu (game started with `skse64_loader.exe`). The image keeps SKSE's few patches.
- `tools/se-port/addrlib.py <version-1-5-97-0.bin> build/se-port/se-ids.csv` decodes the SE
  Address Library (format 1; AE's `versionlib` files are a newer format, use the MCP server's
  exported table in `tools/skyrim-mcp/.cache`).
- AE->SE candidates: `se_ae.csv`, `se-ae-attempted-match.csv` (save as `attempted.csv`) and
  `offsets-1-6-318-0.csv` from github.com/alandtse/skyrim_vr_address_library into
  `build/se-port`, then `tools/se-port/fullmap.py`. They covered all but 7 of ~100 IDs and were
  right wherever checked; CommonLib's pairs differ once (77453, below).
- `tools/se-port/sepair.py` (run from `tools/skyrim-mcp`): `site` finds a call in SE by its
  callee and position among the calls to it, `refs` votes an SE ID for an AE global by the
  functions referencing it, `dis` shows either side.

## Graphics debugging

- RenderDoc: Community Shaders writes captures to `<game>\Data\SKSE\Plugins\CommunityShaders\Captures`
  and they are deleted when RenderDoc closes them, so copy them out first.
- From an agent: turn on "Enable RenderDoc Capture" in Community Shaders' `SettingsUser.json`,
  launch, then bridge `renderdoc_capture`. FSR frame generation breaks afterwards; relaunch.
- Analysis scripts are in `tools/renderdoc/` and run in RenderDoc's own Python:
  `qrenderdoc --python script.py`, with `RDC_CAPTURE` and `RDC_OUT` set.
