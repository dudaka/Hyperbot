# three.js Navmesh Visualization - Status & Handoff

**Status: first cut IMPLEMENTED + two testing/hardening passes done, on macOS.** Region
scopes 1x1 / 3x3 / 5x5 (center region `5c87` = Hotan, plus a ring) render in 3D; interactive
S->G path queries work against Hyperbot's own pathfinder. The code lives in
**`tools/navmesh_viz/`** (standalone C++ backend + `web/` three.js client).

- **Pass 1** fixed three issues (layer-aware height reconstruction, long-segment
  densification, and a cross-region stairway-link bug in `silkroad_lib`).
- **Pass 2 (this session)** root-caused and fixed the **object-stitch vertical "teleport"**:
  stacked terrace->rampart queries jumped ~47-145 units straight up. Cause: in
  `navmeshTriangulation.cpp` both `0x00` and `0x08` object outline edges were treated as
  "the way onto the object", so `0x08` (object<->object) seams were used as terrain->object
  on-ramps. Fix: `0x08` is now blocking from terrain (object<->object link only); only `0x00`
  stitches terrain->object. Also added a blocked-terrain-walk guard, reworked the tool's
  height DP, and added a **compass** + **stitch-edge toggle** + debug hooks. See "Fixed -
  pass 2".

This file is the handoff for the next session, whose job is to **keep testing the GUI path
queries** (more stacked-surface / cross-region / failure cases), run the **Linux `bot`
regression** for the shared `silkroad_lib` changes (top open risk), then push the scaling
work in "Next steps".

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
  `serve` (HTTP). `geometry.cpp` - terrain + BMS extraction to JSON (now also emits each
  object's `outlineEdges` as `[srcVertexIndex, destVertexIndex, flag]` for edge inspection).
  `path.cpp` - Polyanya query, segment densification (~10 units), and a **convex** height DP
  over the stacked surfaces that **excludes blocked terrain** (see "Fixed - pass 2").
  `server.cpp` - cpp-httplib endpoints.
- Data source: the **already-extracted** `sro-data/` loose files (`Data/`, `Map/`,
  `Media/`); only `Data/navmesh/*` is used. The navmesh is parsed by driving
  `NavmeshParser` directly.
- Region scope: center `5c87` + ring radius R (R=0 -> 1 region, R=1 -> 3x3 = 9,
  R=2 -> 5x5 = 25). `serve <data> <R> <port>` loads every ring up to R and caches geometry
  per radius, so the client can switch scope with no reload.
- Endpoints (CORS-enabled, default port 5577): `GET /geometry?r=N` (cached region set for
  ring N), `GET /path?sx&sy&sz&gx&gy&gz` (absolute coords -> waypoints), `GET /info`
  (`{"maxRadius":N}`).

**Frontend - `tools/navmesh_viz/web/` (vanilla three.js + Vite):**

- Renders terrain (height-gradient vertex colors) + BMS objects (per-area colors so floors
  differ), `OrbitControls`, hemisphere + directional light.
- **Region-scope switcher**: 1x1 / 3x3 / 5x5 buttons; switching disposes old meshes,
  fetches `/geometry?r=N`, rebuilds, and reframes the camera. Buttons beyond the server's
  loaded `maxRadius` are disabled.
- S/G picking: `THREE.Raycaster` against terrain + objects; first click = S (green), second
  = G (red), third resets. The **clicked surface Y** is sent through and selects the layer.
- On S+G, calls `/path` and draws the waypoints as a tube hugging the surface.
- **Compass** (added pass 2): a top-right canvas widget that projects the world axes each
  frame so it stays correct through any rotation/pitch (`+Z`=North, `+X`=East; N accented).
  Caveat: Silkroad is left-handed, three.js right-handed, so the scene may be mirrored on one
  axis - cardinals could read flipped vs the in-game compass. One-line fix in `compassDirs`
  if so; it is still a consistent orientation reference regardless.
- **Stitch-edge toggle** (added pass 2): HUD checkbox that overlays object outline edges -
  cyan `0x00` (object<->terrain stitch) and magenta `0x08` (object<->object stitch), drawn
  always-on-top. Off by default. Directly visualizes the distinction behind the pass-2 fix.
- **Debug hooks** (added pass 2, for automated/visual testing): `window.__vizQuery(s, g)`
  places S/G from absolute coords and runs the query; `window.__vizFocus(target, radius,
  azDeg, elDeg)` aims the camera. Used by Playwright; harmless in normal use.
- **On-screen log panel + console logging** (added pass 1): every pick logs its
  surface kind, full-precision absolute/world coords, and NDC; every `/path` call logs the
  request (S, G, URL) and the raw response. A "Clear log" button empties the panel. Invaluable
  for capturing the exact input data behind a misbehaving query.
- **Default scope is 1x1** (lightest load); switch to 3x3 / 5x5 as needed.
- Vite proxies `/geometry`, `/path`, `/info` to `127.0.0.1:5577`.

## How to run / test

```bash
# 1. Backend - load all scopes (R=2) so the UI switcher has 1x1/3x3/5x5.
cd tools/navmesh_viz
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build --target navmesh_viz
./build/navmesh_viz serve ../../sro-data/Data 2 5577     # wait for "Serving on ..."

# 2. Frontend
cd web && npm install && npm run dev
```

Open **<http://localhost:5173/>** (or the next free port Vite prints, e.g. `5174`, if 5173
is taken; use `localhost`, not `127.0.0.1` - Vite binds IPv6).
Drag to orbit; click S then G; use the scope buttons. Quick backend checks:
`curl localhost:5577/info`, `curl 'localhost:5577/geometry?r=0'`,
`curl 'localhost:5577/path?sx=13920&sy=243.6&sz=54240&gx=14880&gy=244&gz=55200'`.

## Decisions (resolved this cut)

- **3D, not 2D** - kept (rationale above). The legacy `ui/navmeshView.*` is the 2D,
  deprecated predecessor.
- **Architecture: Option B** (native HTTP service reusing `silkroad_lib`+`Pathfinder`),
  chosen for lowest risk; it inherits the bridge/link/layer correctness. WASM (Option A)
  remains a possible later optimization.
- **Region scope** is explicit (`5c87` + ring R) rather than auto-selected.
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

## Fixed - pass 2 (this session)

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

## Known gaps / issues (open)

- **Reconstructed waypoints are surface samples, not the true mesh.** With the pass-2 fix +
  ~10-unit densification a climb now follows the actual ramp/stairs surface in small steps
  (no teleport), but it is still a poly-line sampled off the surface, not the mesh itself -
  expect minor visual stair-stepping. Mostly resolved; cosmetic.
- **"No path" is often correct, not a bug** - two picks separated by walls/cliffs/water
  legitimately have no route. Some Pathfinder edge cases also surface as clean errors
  (`"invalid point"`, plus two unrelated Polyanya-internal ones in `third_party/pathfinder`:
  `"createCircleConsciousLine..."`, `"i1=inf..."`) - rare, gracefully surfaced, out of scope
  (vendored lib).
- **Walkable mask** (`terrain.walkable`, 96x96 per region) is sent but not rendered as an
  overlay yet.
- **Debug overlays**: object outline `0x00`/`0x08` stitch edges now have a toggle (pass 2);
  triangulation-triangle and full constraint-edge overlays are still not implemented.
- **Compass cardinals may be mirror-flipped vs the in-game compass** (left-handed Silkroad
  vs right-handed three.js). It is a consistent orientation reference either way; verify E/W
  (or N/S) against the known Hotan layout and flip one axis in `compassDirs` if needed.
- **Per-object draw calls don't scale**: ~250 meshes at R=2 (~31 FPS headless, 12.5 MB
  geometry). Fine for 1x1/3x3/5x5; not for the full ~4021-region map.
- **`silkroad_lib` changes are validated only via the viz on macOS (TOP OPEN RISK).** Five
  navmesh-viz-driven changes have accumulated, three behavior-affecting:
  (1) loose-directory `Pk2ReaderModern` mode *(additive)*;
  (2) `NavmeshParser::setRegionAllowList` *(additive)*;
  (3) cross-region link-traversal fix in `getSuccessors` (pass 1);
  (4) the `0x08` object-stitch fix in `navmeshTriangulation.cpp` (pass 2);
  (5) the blocked-terrain-walk guard in `singleRegionNavmeshTriangulation.cpp` (pass 2).
  3-5 change **shared pathfinding behavior** used by the `bot`. All compile + run via
  `navmesh_viz` on macOS but were **not** built/tested against the Linux `bot` target. Run
  the `bot` gameplay regression before relying on it. In particular, confirm (4) does not
  over-block legitimate object<->object connections whose link data is missing (a known
  "link data does not actually exist" branch in `getSuccessors`).
- **Debug scaffolding kept intentionally** (for ongoing testing): `outlineEdges` in the
  `/geometry` JSON, and the `window.__vizQuery` / `window.__vizFocus` hooks in the web client.
  Strip them before any "production" packaging.

## Next steps

1. **Verify the `silkroad_lib` changes on the Linux `bot` build (top priority / risk).**
   Build the `bot` and run its gameplay regression against the shared-pathfinding changes -
   especially the pass-2 `0x08` object-stitch fix and the blocked-terrain-walk guard, plus
   the pass-1 cross-region link fix. Watch for paths that newly return "no path" where the bot
   previously walked.
2. **Keep testing the UI/GUI queries** (active task): more scope switching, picking on
   stacked surfaces (walls/structures vs ground), cross-region paths, and failure messaging.
   Use the on-screen log panel + the `0x00`/`0x08` toggle to capture the exact data behind any
   oddity. Every fix so far was found this way - expect more (new Polyanya edge cases, other
   fortresses/regions, 5x5 scope).
3. **Confirm/verify the compass cardinals** against the known Hotan orientation; flip one axis
   in `compassDirs` if mirrored.
4. **Optional overlays**: walkable-mask coloring; triangulation-triangle / full
   constraint-edge debug view (stitch-edge overlay already exists).
5. **Scale toward the full map** (the ~4021-region roadmap): merge each region to a few
   draw calls; glTF/glb (or binary) per-region transport with view-driven streaming + LRU
   eviction; LOD/frustum culling; cull empty/water regions; a 2D minimap for overview.
   Pathfinding is already global server-side (one absolute plane spans all loaded regions),
   so only geometry needs streaming.

## Backend entry points (authority: pathfinding.md)

- Parse: `sro::pk2::Pk2ReaderModern{dir}` (loose mode) -> `sro::pk2::NavmeshParser` ->
  `parseNavmesh()` -> `sro::navmesh::triangulation::NavmeshTriangulation`.
- Query: `pathfinder::Pathfinder<NavmeshTriangulation>{tri, cfg}.findShortestPath(S, G)`
  with `cfg = {kPolyanya, agentRadius=3.14, timeout=150ms}`; S/G are absolute `Vector3`.
- Geometry: `Region::tileVertexHeights` (97x97) + `enabledTiles` (96x96);
  `Navmesh::getTransformedObjectResourceForRegion(instanceId, regionId)` for BMS meshes;
  `NavmeshTriangulation::transformRegionPointIntoAbsolute` for the absolute frame.
