# three.js Navmesh Visualization - Status & Handoff

**Status: IMPLEMENTED + three hardening passes + 4th & 5th (viz-only) sessions done, on macOS.**
Region scopes **1x1 .. 23x23** (concentric rings R=0..11 on a single center region `5f82`,
selected from a dropdown), AND **load-by-zone** (any named zone) render in 3D as a **flat
top-down, North-up / East-right map**; interactive S->G path queries work against Hyperbot's
own pathfinder. The code lives in **`tools/navmesh_viz/`** (standalone C++ backend + `web/`
three.js client).

> Passes 1-3 changed shared `silkroad_lib` (committed). Passes 4-5 do **not** change shared
> `silkroad_lib` - they touch only `tools/navmesh_viz/` plus the in-repo `Pathfinder` patch
> (pass 5 added a `canGetToState` reachability guard to it). Pass 5 (this session): single
> center `5f82`, scopes up to 23x23 via a dropdown, the top-down North-up map orientation, the
> reachability fix, the path budget raised to 30s, and a query-time readout + "Reset view"
> button. See "Fixed - pass 4" and "Fixed - pass 5".

- **Pass 1** fixed three issues (layer-aware height reconstruction, long-segment
  densification, and a cross-region stairway-link bug in `silkroad_lib`).
- **Pass 2** root-caused and fixed the **object-stitch vertical "teleport"**:
  stacked terrace->rampart queries jumped ~47-145 units straight up. Cause: in
  `navmeshTriangulation.cpp` both `0x00` and `0x08` object outline edges were treated as
  "the way onto the object", so `0x08` (object<->object) seams were used as terrain->object
  on-ramps. Fix: `0x08` is now blocking from terrain (object<->object link only); only `0x00`
  stitches terrain->object. Also added a blocked-terrain-walk guard, reworked the tool's
  height DP, and added a **compass** + **stitch-edge toggle** + debug hooks. See "Fixed -
  pass 2".
- **Pass 3 (this session)** root-caused and fixed object<->object (`0x08`) **seam crossing**
  plus a Polyanya degeneracy. Where two coplanar object floors abut, their two near-coincident
  link edges form a degenerate sliver "corridor" that (a) connected object A to it at only one
  end - forcing a ~60-unit detour to that corner - and (b) was slow / tripped an unhandled
  Polyanya `"i1/i2 is inf"` degeneracy that aborted the whole search (masked as "No path" by
  the 150ms timeout). Fix: coincident-edge links are now crossed **directly** object-to-object
  wherever the other object is present on the far triangle (like a `0x00` on-ramp), bypassing
  the corridor; the degeneracy is also guarded in `third_party/Pathfinder` (authorized, kept as
  an in-repo patch). This subsumed an earlier radius=0.5 workaround - the viz is back on the
  bot's agent radius 3.14. Added an agent-radius overlay (rings at S/G, read from `/info`). See
  "Fixed - pass 3".

This file is the handoff for the next session, whose job is to run the **Linux `bot`
regression** for the shared `silkroad_lib` changes (top open risk; reapply the combined
`Pathfinder` patch first - it now holds the degeneracy guard AND the reachability guard),
**validate the `kCoincidentSeamEpsilon=8.0` heuristic against real fortress bridges**,
**resume testing the GUI path queries** (stacked-surface / cross-region / `0x08` seam /
zones / failure messaging) across the new large scopes, consider upstreaming the Pathfinder
patch, then push the scaling work in "Next steps". The pass-5 surface (single center `5f82`,
scopes to 23x23, top-down North-up map, reachability guard, 30s budget, query-time + Reset
view) is the newest area to exercise.

Read [pathfinding.md](pathfinding.md) for the backend authority. Tool usage lives in
`tools/navmesh_viz/README.md`.

## Goal (unchanged)

A browser app that (1) renders multiple regions' terrain + BMS object meshes in 3D,
(2) lets you click two points **S** and **G** on a surface (the clicked height picks the
floor/layer), and (3) draws the Polyanya path S->G returned by Hyperbot's pathfinder,
projected onto the surfaces. The 3D requirement is the point: only 3D disambiguates
stacked surfaces (a bridge vs the road under it, floor 1 vs floor 2), which is exactly
what `createStateForPoint` (nearest-altitude) resolves. three.js is Y-up, matching
Silkroad.

## What exists now

**Backend - `tools/navmesh_viz/` (standalone, Option B "C++ HTTP service"):**

- A native binary built **without** the `bot`/Qt/vcpkg/protobuf stack. It compiles a
  subset of `silkroad_lib`'s navmesh sources directly (it excludes `gameData.cpp` to avoid
  the `gli` dependency) plus the `Pathfinder` submodule. Abseil + cpp-httplib come from a
  package manager (e.g. Homebrew). Builds on macOS (and should on Linux).
- `main.cpp` - load + two modes: `dump` (write geometry JSON + a sample path query) and
  `serve` (HTTP). `geometry.cpp` - terrain + BMS extraction to JSON (also emits each
  object's `outlineEdges` as `[srcVertexIndex, destVertexIndex, flag]`; pass 4 added
  `regionIsPassable`, a closed/sealed/non-existent **render filter**). `path.cpp` - Polyanya
  query, segment densification (~10 units), a **convex** height DP over the stacked surfaces
  that **excludes blocked terrain** (see "Fixed - pass 2"), and a **30s timeout** (pass 5; was
  5s) that is reported as `"Search timed out"` distinctly from `"No path found"` (pass 4).
  `zones.cpp` - parses the `textzonename_*.txt` zone table (pass 4). `server.cpp` -
  cpp-httplib endpoints, incl. on-demand per-zone navmesh build + cache.
- Data source: the **already-extracted** `sro-data/` loose files (`Data/`, `Map/`,
  `Media/`); `Data/navmesh/*` for geometry, and `Media/server_dep/silkroad/textdata/` for the
  zone table (pass 4). The navmesh is parsed by driving `NavmeshParser` directly.
- Region scope (pass 5): concentric `(2R+1)x(2R+1)` rings on a **single** center region
  `5f82`, for **R = 0 (1x1) .. 11 (23x23 = 529 cells)**. `serve <data> <R> <port>` loads every
  ring up to R and caches geometry per radius. (Earlier passes used a dual center - `5c87`
  Hotan for R<=3 and `5a87` for the 9x9 - collapsed in pass 5 to the single `5f82`; the
  `clusterRegionIds` loop in `main.cpp` auto-extends to any R, so no per-scope C++ change is
  needed to add radii.) **Zones** are a separate axis: any zone is built + cached on demand (a
  fresh navmesh+triangulation per zone).
- Endpoints (CORS-enabled, default port 5577): `GET /geometry?r=N` (cached ring N) or
  `?zone=NAME` (on-demand zone, pass 4); `GET /zones` (`[{"name","regions"}]`, the 226
  loadable zones; pass 4); `GET /path?sx&sy&sz&gx&gy&gz[&zone=NAME]` (absolute coords ->
  waypoints; with `zone` the query runs in that zone's triangulation); `GET /info`
  (`{"maxRadius":N,"agentRadius":R}` - `agentRadius` = the pathfinder's collision radius,
  added pass 3; `kAgentRadius` lives in `path.hpp`).

**Frontend - `tools/navmesh_viz/web/` (vanilla three.js + Vite):**

- Renders terrain (height-gradient vertex colors) + BMS objects (per-area colors so floors
  differ), `OrbitControls`, hemisphere + directional light.
- **Region-scope switcher (pass 5)**: a **dropdown** of 1x1 .. 23x23 (replaced the old button
  row); selecting disposes old meshes, fetches `/geometry?r=N`, rebuilds, and reframes to the
  default top-down view. Options beyond the server's loaded `maxRadius` are disabled.
- **Zone picker** (pass 4): a type-ahead `<input list=datalist>` populated from `/zones` + a
  "Load zone" button; loading a zone fetches `/geometry?zone=NAME` and sets `activeZone` so
  subsequent `/path` calls carry `&zone=`. Selecting a scope clears `activeZone`
  (back to radius mode). Coexists with the scope dropdown.
- S/G picking: `THREE.Raycaster` against terrain + objects; first click = S (green), second
  = G (red), third resets. The **clicked surface Y** is sent through and selects the layer.
- On S+G, calls `/path` and draws the waypoints as a tube hugging the surface.
- **Compass** (added pass 2, resolved pass 5): a top-right canvas widget that projects the
  world axes each frame so it stays correct through any rotation. Pass 5 made the display a
  deliberate **North-up / East-right map** by mirroring the N-S axis (`world.scale.z = -1`),
  so the rose now matches a conventional map (N at the top, E right); `compassDirs` has N/S
  flipped to suit. The old "may be mirror-flipped" caveat is resolved.
- **Stitch-edge toggle** (added pass 2): HUD checkbox that overlays object outline edges -
  cyan `0x00` (object<->terrain stitch) and magenta `0x08` (object<->object stitch), drawn
  always-on-top. Off by default. Directly visualizes the distinction behind the pass-2 fix.
- **Debug hooks** (added pass 2, for automated/visual testing): `window.__vizQuery(s, g)`
  places S/G from absolute coords and runs the query; `window.__vizFocus(target, radius,
  azDeg, elDeg)` aims the camera. Used by Playwright; harmless in normal use.
- **Agent-radius footprint** (added pass 3): each S/G marker also draws a flat ring of radius
  = the backend's agent radius (fetched from `/info`, 3.14 fallback) so the character's
  collision size is visible to scale against the navmesh. Note: the marker sphere is r=10, so
  the 3.14 ring reads as ~1/3 of it.
- **On-screen log panel + console logging** (added pass 1): every pick logs its
  surface kind, full-precision absolute/world coords, and NDC; every `/path` call logs the
  request (S, G, URL) and the raw response. A "Clear log" button empties the panel. Invaluable
  for capturing the exact input data behind a misbehaving query.
- **Default view (pass 5): a flat top-down, North-up / East-right map.** The camera looks
  straight down `-Y` with `up = -Z`; combined with the N-S mirror, rendered North (local +Z)
  is at the top and East (+X) on the right. Picks invert through `world.worldToLocal`, so
  reported absolute coords stay the true Silkroad values despite the mirror.
- **"Reset view" button (pass 5)**: restores that default top-down framing in one click (it
  clears any orbit/zoom/pan) - useful because picking encourages a lot of rotation. Separate
  from "Reset S/G", which only clears the S/G markers + path.
- **Query-time readout (pass 5)**: the path result status shows the elapsed query time
  (`Path: N waypoint(s) in 21ms ...` / `No path: <error> (8ms)`; >=1s shown as seconds), and
  `elapsedMs` is added to the response log entry.
- **Default scope is 1x1** (lightest load); pick another scope from the dropdown or load a zone.
- Vite proxies `/geometry`, `/path`, `/info`, `/zones` to `127.0.0.1:5577` (`/info` added
  pass 3, `/zones` added pass 4; a proxy entry is required or the client's relative fetch 404s).

## How to run / test

```bash
# 1. Backend - load all scopes (R=11) so the UI dropdown has 1x1..23x23 + zones.
cd tools/navmesh_viz
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build --target navmesh_viz
./build/navmesh_viz serve ../../sro-data/Data 11 5577    # wait for "Serving on ..."
# (R=11 builds 12 cumulative geometry caches on center 5f82 before it listens - ~10-15s; the
#  largest, 23x23, is ~222 MB. Use a smaller R for a faster start when you don't need 23x23.)

# 2. Frontend
cd web && npm install && npm run dev
```

Open **<http://localhost:5173/>** (or the next free port Vite prints, e.g. `5174`, if 5173
is taken; use `localhost`, not `127.0.0.1` - Vite binds IPv6).
The default is a flat top-down North-up map; pick a scope from the dropdown or load a zone,
click S then G, use "Reset view" to re-frame. Quick backend checks:
`curl localhost:5577/info` (`maxRadius:11`), `curl 'localhost:5577/geometry?r=0'`,
`curl localhost:5577/zones` (226 zones), `curl 'localhost:5577/geometry?zone=Jangan'`,
`curl 'localhost:5577/path?sx=9187.7&sy=-92.7&sz=79470.7&gx=9800&gy=-92&gz=79000'` (a path).

## Decisions (resolved this cut)

- **3D, not 2D** - kept (rationale above). The legacy `ui/navmeshView.*` is the 2D,
  deprecated predecessor.
- **Architecture: Option B** (native HTTP service reusing `silkroad_lib`+`Pathfinder`),
  chosen for lowest risk; it inherits the bridge/link/layer correctness. WASM (Option A)
  remains a possible later optimization.
- **Region scope** is explicit (single center `5f82` + ring R, pass 5) rather than
  auto-selected. The center is a `main.cpp` constant; the dual-center scheme (Hotan `5c87` +
  `5a87`) was collapsed to one center in pass 5.
- **Display orientation (pass 5): a flat top-down North-up / East-right map.** Silkroad is
  left-handed and three.js right-handed, so from an above view you cannot have *both* North-up
  and East-right without mirroring one axis; the viz mirrors N-S (`world.scale.z = -1`). This
  is **display only** - the wire frame and reported absolute coords are unchanged.
- **Transport** is custom JSON for now (glTF/glb is a scaling option - see Next steps).
- **Coordinate frame on the wire**: the absolute pathfinder plane (origin region 16512),
  Y-up. Terrain, objects, and path waypoints all use
  `transformRegionPointIntoAbsolute`, so client picks line up with the pathfinder.

## Key findings (so they aren't rediscovered)

- `GameData::parseSilkroadFiles` has `parseNavmeshData` **commented out** (a `triangle.c`
  leak). The viz therefore does **not** use the full `GameData` path; it drives
  `NavmeshParser` directly and only needs the `Data/navmesh` side.
- `NavmeshParser::regionIsEnabled` has a hardcoded "interesting-area" whitelist that does
  **not** include `5c87`/Hotan. Added an **additive** `NavmeshParser::setRegionAllowList`
  (default behavior preserved) to target an explicit region set.
- `Pk2ReaderModern` gained an **additive loose-directory mode**: constructed with a
  directory it serves loose files (`sro-data/`) instead of a `.pk2` archive; the public
  interface is unchanged.
- **macOS-only Pathfinder portability bug**: `third_party/pathfinder/hash_combine.h`'s
  `distribute(std::hash<T>{}(v))` is ambiguous because `size_t` (unsigned long) is distinct
  from `uint64_t` (unsigned long long); on Linux they're the same type. Worked around with
  a SFINAE shim in `path.cpp` **without modifying `third_party/`** (it must be declared
  before the Pathfinder headers because the call is a qualified-id).
- Pathfinder's `findShortestPath` returns `PathfindingResult` owning `unique_ptr` segments;
  raw segment pointers must be consumed **while it is alive** (an earlier use-after-free
  produced `(0,0)` waypoints).
- **Polyanya emits only corner waypoints, with height ~0** - it returns `StraightPathSegment`s
  (2D `x`,`z`) between turns, nothing in between. A single straight segment can span terrain
  whose height swings widely, so any 3D consumer must densify + reconstruct height (we do).
- **Cross-region stairway links are triangulated inside ONE region.** When a link joins
  objects in two regions, the region that "absorbs" both objects holds the whole link (both
  src + dest edges, and the two objects overlap on the shared exit triangle); the other region
  may have an empty link map. The stitched region border can still let the A* search step into
  that other region mid-link. This drove the pass-1 link bug.
- **Object outline-edge flag semantics (the pass-2 root cause).** `EdgeFlag` (navmesh.hpp):
  `kBlockDst2Src=1`, `kBlockSrc2Dst=2`, `kBlocked=3`, `kInternal=4`, `kGlobal=8`,
  `kBridge=16`, `kEntrance=32`, `kSiege=128`. For an OBJECT's outline (perimeter) edges:
  **`0x00` = stitched to terrain** (the legitimate terrain<->object on-ramp, heights match);
  **`0x08` = stitched to another object** (object<->object, traversed only via its link).
  Hotan 3x3 has ~344 `0x00`, ~10101 `0x03` (blocked walls/railings), 14 `0x08`, 2 `0x10`
  (bridge). The old code treated `0x00` and `0x08` identically -> terrain->object jumps.
- **Blocked terrain is a per-triangle flag, not an edge.** Unwalkable tiles
  (`enabledTiles=false`, e.g. a fortress wall face) are recorded only as
  `blockedTerrainTriangles_`; interior blocked cells get **no** constraint edge (only
  region-boundary edges, via `buildGlobalEdgesBasedOnBlockedTerrain`). `getSuccessors`
  historically checked the flag only when *leaving an object* / at start+goal - so an
  on-terrain walk could cross blocked terrain. Pass 2 added a guard (see "Fixed - pass 2").
- **The pathfinder result carries no surface state.** `StraightPathSegment` exposes only 2D
  `x,z`; the A* layer (terrain vs object) is internal and not returned, and
  `third_party/pathfinder` must not be modified. So the viz **reconstructs** height itself.
  The `bot` also does not compute real path heights (waypoints built from a `y=0` input; the
  server snaps movement to the ground) - there is no authoritative height logic to copy.
- **A Polyanya timeout returns an EMPTY result, not an error (pass-4 root cause).** When the
  150ms deadline fired, `Pathfinder::findShortestPath`'s reachability pre-check
  (`canGetToState`) returned `false` -> `findShortestPath` returned an empty path, which
  `path.cpp` reported as `"No path found"` - **indistinguishable from a genuine disconnect**.
  The only way to tell them apart is to time the call. So treat any pre-pass-4 "No path"
  observation as suspect; a real disconnect rejects near-instantly, a timeout runs the full
  budget. (The viz now uses 30s + labels timeouts; the `bot` keeps 150ms.)
- **`canGetToState` had a re-expansion blow-up - the real cause of "starts-on-an-object times
  out" (pass-5 root cause).** The reachability pre-check is a greedy best-first triangle search
  that re-enqueued states already on the heap and had **no closed-set skip at pop**, so the
  same states were re-expanded thousands of times. Diagnosed with an expansion counter (since
  removed): starting on dense fortress object geometry hit **~20M expansions** over only
  ~thousands of distinct states (heap into the millions) and **timed out before Polyanya even
  ran** - returning empty, reported as "no path"/"timeout"; starting the same pair from open
  terrain happened to reach the goal first and was fast. The earlier asymmetric timeout was
  **this**, not a Polyanya issue. Fixed by a one-line closed-set guard (in the Pathfinder
  patch): those queries drop from a 5s timeout to ~0.5s. NOT to be confused with the next item.
- **Polyanya itself is genuinely O(millions of expansions) for very long paths (pass-5
  finding).** A real, optimal path crossing ~20 regions of fine terrain needed **~3.9M interval
  expansions and ~22s** (reachability confirmed connected in ~2k expansions; `expansions ==
  visited`, so no re-expansion - inherent scale). This is *why* the viz budget is now 30s, and
  it is a different phenomenon from the `canGetToState` bug above (re-expansion vs distance). A
  long cross-mesh query therefore legitimately blocks the UI ~20s before the path appears.
- **Handedness / orientation is now pinned (pass-5 finding).** three.js is right-handed +
  Y-up; Silkroad is left-handed with `+Z`=North, `+X`=East. A top-down view cannot show both
  North-up and East-right without mirroring one horizontal axis - so the viz mirrors N-S
  (`world.scale.z = -1`) for a conventional map, and inverts picks via `world.worldToLocal` to
  keep absolute coords true. (Resolves the old "compass may be mirror-flipped" caveat.)
- **`NavmeshTriangulation` uses a fixed origin region 16512** (`navmeshTriangulation.hpp`,
  `setOriginRegion`/default) for its absolute<->region transforms, **independent of which
  regions are loaded**. This is why per-zone triangulations (each built from only that zone's
  regions) all share one global coordinate frame: a zone far from Hotan renders centered and
  its S->G coords round-trip correctly (verified: Jangan renders at abs ~(76000, _, 63000)).
- **Zone table format (`textzonename_*.txt`, pass 4).** UTF-16 LE, tab-delimited; **field
  index 2 = decimal region id**, **field index 9 = English zone name**; a zone is the set of
  regions sharing one English name. Of **655** distinct names, only **226 are loadable**: the
  other 429 are **instanced dungeons** whose index-2 column is a symbolic codeName
  (`DH_A01_F01_ROOM_01`), not a number, AND this extract has **no dungeon navmesh at all**
  (`nv_[8-f]*.nvm` count = 0), so they are unrenderable regardless. `zones.cpp` parses index-2
  numerically and skips the rest. (silkroad_lib's `TextZoneNameData` parser was NOT reused: it
  uses a different column offset and exposes only a forward `regionId->name` map.)

## Fixed - pass 1

- **Layer-aware path height (DP over stacked surfaces).** `path.cpp` `reconstructHeights`
  enumerates every stacked surface at each waypoint's column (terrain + each object
  `(instance, area)` floor via `ObjectResource::getHeight`), then picks one surface per
  waypoint via a DP anchored to the clicked start/goal heights. Beats a greedy nearest-altitude
  walk (which stays stuck on terrain and never climbs). *(Pass 2 reworked the DP cost - see
  below.)*
- **Segment densification.** Polyanya emits only corner waypoints, so one straight segment can
  span widely-varying height; sampling height only at the corners drew a chord that speared
  through the ground. `path.cpp` subdivides each straight segment into short steps before the
  height DP so the path hugs the surface. *(Pass 2 tightened the step to ~10 units.)*
- **Cross-region stairway-link traversal (core `silkroad_lib`).** Picks whose route crossed a
  link joining objects in two regions could `throw` out of `getSuccessors` and abort the whole
  A* search (`"...both objects to be on this triangle"`, `"We have no list of triangles for
  this link"`). Root cause (proven via diagnostics, NOT a partial-load artifact): see the
  cross-region-link finding above. Fix in `singleRegionNavmeshTriangulation.cpp`
  `getSuccessors`: choose the link-exit object from the *exit edge's own link side*
  (`isOnSourceSideOfLink`) instead of guessing from which objects sit on the triangle; and when
  the link is unknown in the current region, yield no successor instead of aborting. Validated
  on macOS via the viz (runs this exact code): failing picks no longer crash, link exits
  succeed, regressions hold, a 400-query random sweep triggers none of the link throws. Some
  picks now correctly report "No path" (verified genuinely unreachable). **Pending a Linux
  `bot` gameplay-regression pass** since it changes shared pathfinding.

## Fixed - pass 2

- **Object-stitch vertical "teleport" (core `silkroad_lib`, the headline fix).** Stacked
  terrace->rampart picks jumped ~47-145 units straight up onto a raised object floor. Proven
  root cause: in `navmeshTriangulation.cpp` the object outline-edge handler treated both
  `0x00` and `0x08` as "non-blocking, our way onto the object", so a `0x08` (object<->object)
  seam was usable as a terrain->object on-ramp. Fix: `0x08` edges now get
  `kGlobal | kBlocking` (blocking from the terrain side; the object<->object **link** still
  works because link handling runs *before* the blocking check on the object side); only
  `0x00` edges remain terrain on-ramps. Verified via the viz: the failing terrace->rampart
  query went from 95 wp with a 47-unit jump to **439 wp, max single step 10** (it reroutes to
  a real `0x00` entrance and climbs a gradual ramp); the older 145-jump query dropped to 6.3;
  flat/short/terrace->object regressions unchanged (max steps 0-1.4).
- **Blocked-terrain-walk guard (core `silkroad_lib`).** In
  `singleRegionNavmeshTriangulation.cpp` `getSuccessors`, a successor that stays on terrain
  may no longer enter a `blockedTerrainTriangles_` triangle (those carry no constraint edge,
  so an on-terrain walk could otherwise cross a wall). Stepping onto an *object* over the same
  cell is still allowed. Independent correctness fix; did not change the headline query (which
  confirmed that path was not crossing blocked terrain).
- **Tool height-DP rework (`path.cpp`).** Three changes that make the viz robust regardless of
  upstream: (a) `surfaceHeightsAtPoint` drops the terrain candidate where `enabledTiles` is
  false (never rest a path on a blocked wall face); (b) the DP step cost is now the **squared**
  height difference (convex) instead of `|dY|` - L1 telescopes and is indifferent to one big
  teleport vs a spread climb, so it freely jumped; squared strictly prefers a gradual climb;
  (c) densification tightened to ~10 units so steep transitions get sampled.
- **Inspection tooling.** `geometry.cpp` now emits per-object `outlineEdges` (with flags);
  the web client adds the **compass**, the **stitch-edge toggle** (cyan `0x00` / magenta
  `0x08`), and the `__vizQuery`/`__vizFocus` debug hooks (all described in "What exists now").

## Fixed - pass 3 (this session)

- **Object<->object (`0x08`) coincident-seam crossing (core `silkroad_lib`, the headline
  fix).** Two coplanar object floors that abut (e.g. two terrace slabs meeting) are stitched by
  a `0x08` link whose two object outline edges are **near-coincident** (in Hotan: ~0.5-1.7
  units apart, running along the same seam line). The link's traversable area is the triangulated
  patch *between* those two edges; when they nearly coincide that patch is a degenerate sliver
  wedge that touches object A at only one end. Symptoms: (1) crossing the seam funneled through
  that one corner, so a straight S->G across the seam detoured ~60 units out and back; (2) the
  same dense degenerate geometry tripped a Polyanya numeric degeneracy (below) and, before that
  surfaced, simply timed out (150ms) as a spurious "No path". Proven root cause via diagnostics:
  S/G resolved onto two different stacked objects (A=`1569139713`, B=`1585925122`), linked by
  link id 1; the search *did* reach the goal triangle on B but the corridor entry was pinned to
  the seam's west endpoint. **Fix** (`singleRegionNavmeshTriangulation.{hpp,cpp}`):
  `buildLinkData` flags a link `edgesCoincident` when both endpoint gaps between its src/dest
  edges are `< kCoincidentSeamEpsilon` (8.0 units); in `getSuccessors`, crossing a coincident
  link edge now steps **directly onto the other object** wherever that object is present on the
  far triangle (exactly like a `0x00` terrain<->object on-ramp), so Polyanya takes the straight
  crossing instead of the corridor. Separated-edge links (real bridges) are not flagged and keep
  the original corridor traversal. Verified via the viz at the bot's radius **3.14**: the
  same-X probe stays at X=14584.5 end-to-end (no detour); the original ramp query's X-spread
  dropped from ~60 to 0.8; height stays continuous across the seam (0.29-unit floor step, no
  teleport); a 702-query grid spanning both objects + the seam is **702/702 ok, 0 crashes,
  <=23ms**.
- **Polyanya "i1/i2 is inf" degeneracy crash (`third_party/Pathfinder/pathfinder.h`).** In the
  dense near-coincident seam geometry, the interval projection in
  `doesRightIntervalIntersectWithLeftOfSuccessorEdge` / `...Left...Right...` hit an unhandled
  "parallel edges closer than the agent radius" case and **threw**, aborting the whole search.
  At radius 3.14 + 150ms this masked as "No path"; with a longer timeout it surfaced as the
  exception. Fix: the two throw sites (`[0]` and its mirror `[2]`) now run the library's own
  "reachable only if we can turn around the constraint, else skip this successor" logic instead
  of throwing - regression-safe by construction (only previously-crashing inputs change). This
  edits the vendored submodule (an **authorized exception** to "do not modify `third_party/`");
  it is preserved in `tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch` (which since
  pass 5 also bundles the reachability guard; apply with `cd third_party/Pathfinder && git
  apply ../../tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch`).
  **Now optional**: the seam fix bypasses the degenerate corridor, so this is no longer reached
  for the known cases (702/702 pass even with the guard reverted) - kept as a safety net and an
  upstream candidate.
- **Agent radius is no longer a viz workaround.** During investigation, radius=0.5 "fixed" the
  no-path by dodging the degeneracy; that was a workaround that diverged from the bot. With the
  seam fix the viz is back on the bot's `kAgentRadius = 3.14` (now in `path.hpp`, exposed via
  `/info`). Changing the viz radius is a one-line edit in `path.hpp`; it is intentionally
  separate from the bot's `bot.cpp` value (both 3.14).
- **Agent-radius overlay + `/info` plumbing.** `server.cpp` `/info` now reports `agentRadius`;
  the web client draws a footprint ring at S/G from that value (proven backend-driven: setting
  the backend to 9.0 made the rings render at radius 9.0). Also fixed a latent bug - `/info`
  was never in the Vite proxy, so the client had always fallen back to its default
  `maxRadius`/radius; `vite.config.js` now proxies `/info`.

## Fixed / added - pass 4 (this session, viz-only - no shared `silkroad_lib` change)

- **Path-timeout fix + honest labeling (`path.cpp`).** The viz raised its own Polyanya
  timeout from 150ms to **5s** (`kPathTimeout`) and now distinguishes `"Search timed out
  (>N ms)"` from `"No path found"` by timing the call (the library returns an empty result on
  timeout either way - see the pass-4 finding above). **Proven**: a query that reported "No
  path" at 150ms (pinned at ~164ms) returns a valid 838-waypoint path in ~2.4s with the larger
  budget. Rationale: the viz is an interactive debugging tool, so a correct answer in a couple
  of seconds beats a fast wrong "No path". (At the time no realistic query exceeded 5s, so the
  `"Search timed out"` branch was unexercised; **pass 5 later raised the budget to 30s** after
  a 23x23 cross-mesh query did exceed 5s - so that branch is now exercised.)
- **9x9 scope recentered on `5a87` (`main.cpp`).** Added R=4 with a per-scope center (R<=3 ->
  `5c87`, R=4 -> `5a87`) and a **union** parser allow-list so both clusters load. The 9x9 is
  **not concentric** - switching into it shifts the view ~2 regions south (a deliberate,
  user-confirmed choice). Verified: 9x9 loads 73 regions (8 of the 81 cells are non-existent
  map-edge regions).
- **Closed/sealed/non-existent render filter (`geometry.cpp`, `regionIsPassable`).** A region
  is rendered only if it has >=1 walkable tile AND (when other regions share the view) >=1
  usable - not fully blocked - global edge to a loaded neighbor; the single-region case is
  guarded so a 1x1 is never blanked. Non-existent regions were already skipped. **Verified no
  false positives** (r=2 -> 25/25, r=3 -> 49/49 unchanged; r=4 -> 73/81 where all 8 dropped are
  non-existent). The closed/sealed **true-positive path is NOT exercised** by the current
  clusters (no loaded region is actually closed/sealed) - it is in place but unproven on live
  data.
- **Load-by-zone (`zones.cpp` + `server.cpp` + web).** New `/zones`, `/geometry?zone=NAME`,
  and `/path?...&zone=NAME`. Zones build on demand - a fresh `Navmesh` + `NavmeshTriangulation`
  + geometry per zone, cached forever behind a mutex (move-assign avoids a deep `Navmesh`
  copy); the cache is **node-stable** so a returned pointer stays valid for lock-free path
  queries. The web client adds a type-ahead zone picker. Verified end-to-end: 226 zones listed,
  Jangan (6 regions) renders and an in-zone S->G path returns ok; first build ~0.17s, cached
  ~0.005s. The closed/sealed filter applies to zone regions too.

## Fixed / added - pass 5 (this session)

- **Reachability re-expansion guard (`Pathfinder` patch, the headline backend fix).** The
  `canGetToState` pre-check re-expanded states thousands of times (no closed-set skip at pop),
  timing out the whole query before Polyanya ran - the true cause of the earlier asymmetric
  "object-start times out, terrain-start is fast" puzzle. One-line `if (visited.find(...) !=
  end()) continue;` at pop fixes it; the prior 5s-timeout object-start queries return in ~0.5s.
  Folded into `tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch` (renamed from
  `...-degeneracy-guard.patch`; now bundles both the pass-3 degeneracy guard and this guard).
  See "Key findings" for the diagnosis. **Still pending the Linux `bot` regression** (it edits
  shared Pathfinder, like the degeneracy guard).
- **Path budget 5s -> 30s (`path.cpp`), after proving long paths are scale, not a bug.** A real
  ~20-region path needs ~3.9M Polyanya expansions / ~22s; the old 5s budget cut it off and
  mislabeled it a timeout. 30s lets such paths complete. (The `"Search timed out"` label is now
  genuinely exercised - a 23x23 cross-mesh query did exceed 5s before the raise.)
- **Single center `5f82`, scopes to 23x23 via a dropdown (`main.cpp`, `web/`).** Collapsed the
  dual-center (`5c87`/`5a87`) scheme to one center `5f82`; extended the loadable scopes to
  **R=11 (23x23)** and replaced the scope buttons with a `<select>` dropdown (auto-disables
  options above the server's `maxRadius`). Verified region render counts (after the
  closed/sealed/non-existent filter): 3x3 8/9, 13x13 141/169, 21x21 394/441, 23x23 463/529 -
  `5f82` sits near the **north map edge**, so large scopes truncate northward.
- **Flat top-down North-up / East-right map orientation (`web/`).** N-S display mirror
  (`world.scale.z = -1`) + straight-down camera (`up = -Z`) so North is at the top and East
  on the right; pick->absolute inverts via `world.worldToLocal` (keeps true coords); compass
  `compassDirs` N/S flipped to match. Verified: a center pick on `5f82` returns region `0x5f82`,
  and a SW-bound path renders toward the lower-left.
- **Query-time readout + "Reset view" button (`web/`).** The path status shows elapsed time
  (and `elapsedMs` is logged); "Reset view" re-frames to the default top-down in one click
  (distinct from "Reset S/G").

## Known gaps / issues (open)

- **(pass 4) Zone cache is unbounded** - every visited zone keeps its navmesh + triangulation
  + geometry forever. Fine in practice (median zone = 1 region; only 14 zones > 50, max 254),
  but the few big zones (`Mirror Dimension` 254, the shipwreck zones 224) cost a multi-second
  **first** load that blocks while building (the build holds a mutex). Add an LRU only if
  memory becomes an issue.
- **(pass 4) Instanced-dungeon zones (429 of 655) are unsupported** - their index-2 region id
  is a symbolic codeName and this extract has no dungeon navmesh, so they are unrenderable.
  Supporting them would need a codeName->region-id map AND dungeon `nv_8*.nvm` data.
- **(pass 4) Closed/sealed filter true-positive is unproven on live data** - no loaded region
  is actually closed/sealed, so that branch of `regionIsPassable` is correct by construction
  but unexercised; watch for it when testing other regions/zones. (The `"Search timed out"`
  label, previously also unproven, *is* now exercised - pass 5 raised the budget to 30s after a
  real 23x23 query exceeded 5s.)
- **Reconstructed waypoints are surface samples, not the true mesh.** With the pass-2 fix +
  ~10-unit densification a climb now follows the actual ramp/stairs surface in small steps
  (no teleport), but it is still a poly-line sampled off the surface, not the mesh itself -
  expect minor visual stair-stepping. Mostly resolved; cosmetic.
- **"No path" is often correct, not a bug** - two picks separated by walls/cliffs/water
  legitimately have no route. Some Pathfinder edge cases also surface as clean errors
  (`"invalid point"`, `"createCircleConsciousLine..."`). The `"i1=inf..."` Polyanya degeneracy
  that used to **abort** the whole search (and so masquerade as "No path") is now **guarded**
  (pass 3 - see "Fixed - pass 3"; patched in the vendored lib). Watch for the un-guarded sibling
  `createCircleConsciousLine` case during further testing.
- **Coincident-seam heuristic is unvalidated against real bridges (pass-3 risk).**
  `kCoincidentSeamEpsilon = 8.0` (units) classifies a `0x08` link as a direct crossable seam
  vs a corridor bridge by its src/dest edge-endpoint gap. Validated only on Hotan's stitches
  (gaps ~0.5-1.7) - **not** against genuine bridge links (the bandit/jangan fortress bridges
  the code's own sanity checks mention). If a real bridge's facing edges sit < 8 units apart it
  would be misclassified coincident and crossed directly (wrong). Validate before scaling; the
  threshold may need to also consider height delta.
- **Underlying causes still present, only bypassed.** The seam corridor is degenerate because
  (a) `linkDataMap_[id].accessibleTriangleIndices` omits tiny triangles below the
  `trianglesOverlap` epsilon (the code's own "Big TODO" in `buildLinkData`), and (b)
  `objectDatasForTriangles_` overlap registration is incomplete where object floors stack. The
  coincident-seam fix sidesteps both for abutting floors; the general triangle-overlap TODO is
  unfixed.
- **Walkable mask** (`terrain.walkable`, 96x96 per region) is sent but not rendered as an
  overlay yet.
- **Debug overlays**: object outline `0x00`/`0x08` stitch edges now have a toggle (pass 2);
  triangulation-triangle and full constraint-edge overlays are still not implemented.
- **(pass 5) Center `5f82` is near the north map edge** - so large scopes truncate northward
  (13x13 renders 141/169, 21x21 394/441, 23x21 463/529 cells; the rest are non-existent
  edge/sea regions). A more interior center would fill the big squares evenly; change the one
  `kCenterRegionId` constant in `main.cpp` (it must be a present, walkable region or startup
  aborts).
- **(pass 5) Large scopes are heavy**: 23x23 geometry is **~222 MB** of JSON and a cold browser
  load of that (parse + GPU upload of hundreds of regions) is slow and memory-hungry; serving
  R=11 also makes the backend spend ~10-15s building all 12 caches before it listens. Small
  scopes stay snappy. A long cross-mesh path query also blocks the UI ~20s (the client just
  awaits the 30s-budget fetch). All acceptable for a debug tool; revisit with the scaling work.
- **Per-object draw calls don't scale**: ~250 meshes at R=2 (~31 FPS headless, 12.5 MB
  geometry). Fine for the smaller scopes and the (mostly small) zones; the large pass-5 scopes
  (23x23 ~222 MB) and the full ~4021-region map need the merge/stream work in "Next steps".
- **`silkroad_lib` changes are validated only via the viz on macOS (TOP OPEN RISK).** Six
  navmesh-viz-driven changes have accumulated, four behavior-affecting:
  (1) loose-directory `Pk2ReaderModern` mode *(additive)*;
  (2) `NavmeshParser::setRegionAllowList` *(additive)*;
  (3) cross-region link-traversal fix in `getSuccessors` (pass 1);
  (4) the `0x08` object-stitch fix in `navmeshTriangulation.cpp` (pass 2);
  (5) the blocked-terrain-walk guard in `singleRegionNavmeshTriangulation.cpp` (pass 2);
  (6) the coincident-seam direct-crossing in `singleRegionNavmeshTriangulation.cpp` (pass 3).
  3-6 change **shared pathfinding behavior** used by the `bot`. All compile + run via
  `navmesh_viz` on macOS but were **not** built/tested against the Linux `bot` target. Run
  the `bot` gameplay regression before relying on it. In particular: confirm (4) does not
  over-block legitimate object<->object connections whose link data is missing (a known
  "link data does not actually exist" branch in `getSuccessors`); and confirm (6) does not
  open a crossing where the bot expected a corridor bridge (the epsilon risk above).
- **`third_party/Pathfinder` is modified** by **two** fixes now - the pass-3 Polyanya
  degeneracy guard AND the pass-5 `canGetToState` reachability guard - both in one in-repo
  patch `tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch`, **not** a submodule bump,
  so a fresh `git submodule update` reverts them. Reapply with `cd third_party/Pathfinder &&
  git apply ../../tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch` (or upstream and
  bump the submodule properly) on the Linux box before the `bot` regression. Currently applied
  in this working tree.
- **Debug scaffolding kept intentionally** (for ongoing testing): `outlineEdges` in the
  `/geometry` JSON, and the `window.__vizQuery` / `window.__vizFocus` hooks in the web client.
  Strip them before any "production" packaging.

## Next steps

1. **Verify the `silkroad_lib` + `Pathfinder` changes on the Linux `bot` build (top priority /
   risk).** Reapply the combined Pathfinder patch (`pathfinder-polyanya-fixes.patch` - both the
   degeneracy and reachability guards) first, then build the `bot` and run its gameplay
   regression against the shared-pathfinding changes - the pass-3 coincident-seam
   direct-crossing and the pass-2 `0x08` object-stitch fix + blocked-terrain guard, plus the
   pass-1 cross-region link fix, plus the pass-5 reachability guard. Watch for paths that newly
   return "no path" where the bot previously walked, and for seam crossings opened where a
   corridor bridge was intended. (Note: the `bot`'s 150ms budget still masks timeouts as "no
   path"; the reachability guard should cut its reachability-pre-check cost too.)
2. **Resume testing the UI/GUI queries** (active task): exercise the new large scopes (up to
   **23x23**) and zone loading, picking on stacked surfaces (walls/structures vs ground),
   cross-region paths, the now-fixed `0x08` seam cases, and failure messaging ("No path" vs
   "Search timed out", with the new query-time readout). Use the on-screen log panel + the
   `0x00`/`0x08` toggle to capture the exact data behind any oddity. Every fix so far was found
   this way - expect more (un-guarded `createCircleConsciousLine`, other fortresses/regions,
   distant zones, the closed/sealed filter on a genuinely sealed region). **Reproducible pass-3
   seam test** (was on the Hotan `5c87` center; reachable from a center that includes it):
   objects A=`1569139713` (Y~146.6) and B=`1585925122` (Y~146.3) meet along a `0x08` seam at
   absolute Z~57297 spanning X 14523..14648 (link id 1). Straight crossing
   `S(14584.5,146.64,57137) -> G(14584.5,146.348,57310)` must stay at X~14584.5 (no detour);
   the up-the-ramp goal is `G(14583.7,177.6,57629)`. A grid of `/path` queries spanning both
   objects + the seam is a good smoke test (702/702 ok at radius 3.14).
3. **Optional overlays**: walkable-mask coloring; triangulation-triangle / full
   constraint-edge debug view (stitch-edge overlay already exists).
4. **Polish load-by-zone** (pass 4 follow-ups): bound the zone cache (LRU); a background /
   non-blocking build for the few big zones; optionally a codeName->region-id map + dungeon
   `nv_8*.nvm` data to surface the 429 instanced-dungeon zones.
5. **Scale toward the full map** (the ~4021-region roadmap): merge each region to a few
   draw calls; glTF/glb (or binary) per-region transport with view-driven streaming + LRU
   eviction; LOD/frustum culling; cull empty/water regions; a 2D minimap for overview.
   Pathfinding is already global server-side (one absolute plane spans all loaded regions);
   the per-zone on-demand build + cache (pass 4) is a first step toward this streaming model.

## Backend entry points (authority: pathfinding.md)

- Parse: `sro::pk2::Pk2ReaderModern{dir}` (loose mode) -> `sro::pk2::NavmeshParser` ->
  `parseNavmesh()` -> `sro::navmesh::triangulation::NavmeshTriangulation`.
- Query: `pathfinder::Pathfinder<NavmeshTriangulation>{tri, cfg}.findShortestPath(S, G)`
  with `cfg = {kPolyanya, agentRadius=3.14, timeout=30s in the viz / 150ms in the bot}`; S/G
  are absolute `Vector3`. A timeout returns an **empty** result (looks like "no path") - the
  viz times the call to label it.
- Geometry: `Region::tileVertexHeights` (97x97) + `enabledTiles` (96x96);
  `Navmesh::getTransformedObjectResourceForRegion(instanceId, regionId)` for BMS meshes;
  `NavmeshTriangulation::transformRegionPointIntoAbsolute` for the absolute frame.
