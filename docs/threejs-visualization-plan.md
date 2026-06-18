# three.js Navmesh Visualization - Status & Handoff

**Status: first cut IMPLEMENTED + one testing/hardening pass done, on macOS.** Region scopes
1x1 / 3x3 / 5x5 (center region `5c87` = Hotan, plus a ring) render in 3D; interactive S->G
path queries work against Hyperbot's own pathfinder. The code lives in **`tools/navmesh_viz/`**
(standalone C++ backend + `web/` three.js client). The first testing pass found and **fixed
three issues** (layer-aware height reconstruction, long-segment densification, and a
cross-region stairway-link pathfinding bug in `silkroad_lib`) - see "Fixed this testing
pass". This file is the handoff for the next session, whose job is to **keep testing the GUI
path queries** (more stacked-surface / cross-region / failure cases) and then push the
scaling work in "Next steps".

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
  `serve` (HTTP). `geometry.cpp` - terrain + BMS extraction to JSON. `path.cpp` - Polyanya
  query, segment densification, and a minimal-vertical-movement height DP over the stacked
  surfaces (see "Fixed this testing pass"). `server.cpp` - cpp-httplib endpoints.
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
- **On-screen log panel + console logging** (added this testing pass): every pick logs its
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
  that other region mid-link. This drove the link bug fixed this pass.

## Fixed this testing pass

- **Layer-aware path height (minimal-vertical-movement DP).** `path.cpp` `reconstructHeights`
  enumerates every stacked surface at each waypoint's column (terrain + each object
  `(instance, area)` floor via `ObjectResource::getHeight`), then picks one surface per
  waypoint so total `|dY|` along the path is minimized, anchored to the clicked start/goal
  heights. Beats a greedy nearest-altitude walk (which stays stuck on terrain and never
  climbs). Verified: a goal on a raised wall (304) now climbs terrain -> object floor -> 304
  (greedy left it terrain-snapped at 243).
- **Segment densification.** Polyanya emits only corner waypoints, so one straight segment can
  span widely-varying height; sampling height only at the corners drew a chord that speared
  through the ground (an S->G segment dropping 244->119 cut diagonally through everything).
  `path.cpp` now subdivides each straight segment into ~30-unit steps before the height DP, so
  the path hugs the surface (that case now descends 244->226->...->119 over ~270 units; this
  also fixed a start-height-off-by-14 artifact, since the DP no longer lowers the start to
  cheat one big jump).
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

## Known gaps / issues (open)

- **Path mount-step is a single straight segment.** Reconstructed waypoints are surface
  *samples*, not the true mesh, so the one segment where a path mounts a structure (stairs /
  ramp) is still a straight climb - the ramp geometry isn't represented. Minor / visual.
- **"No path" is often correct, not a bug** - two picks separated by walls/cliffs/water
  legitimately have no route. Some Pathfinder edge cases also surface as clean errors
  (`"invalid point"`, plus two unrelated Polyanya-internal ones in `third_party/pathfinder`:
  `"createCircleConsciousLine..."`, `"i1=inf..."`) - rare, gracefully surfaced, out of scope
  (vendored lib).
- **Walkable mask** (`terrain.walkable`, 96x96 per region) is sent but not rendered as an
  overlay yet.
- **Debug overlays** (triangulation triangles / constraint edges) are not implemented.
- **Per-object draw calls don't scale**: ~250 meshes at R=2 (~31 FPS headless, 12.5 MB
  geometry). Fine for 1x1/3x3/5x5; not for the full ~4021-region map.
- **`silkroad_lib` changes are additive but macOS-only so far.** Three changes total:
  loose-directory `Pk2ReaderModern` mode, `NavmeshParser::setRegionAllowList`, and the
  cross-region link-traversal fix in `getSuccessors`. All compile + run via `navmesh_viz` on
  macOS but were **not** built/tested against the Linux `bot` target - verify there before
  relying on the bot (the link fix changes shared pathfinding behavior).

## Next steps

1. **Keep testing the UI/GUI queries** (active task): more scope switching, picking on
   stacked surfaces (walls/structures vs ground), cross-region paths, and failure messaging.
   Use the on-screen log panel to capture the exact S/G/URL/response behind any oddity. The
   three issues already fixed (height, densification, links) were all found this way -
   expect to find more (e.g. the mount-step residual, or new Polyanya edge cases).
2. ~~**Layer-aware path height** so multi-floor routes follow the correct surface.~~ Done
   (DP + densification in `path.cpp`); only the 2D-waypoint mount step remains.
3. **Optional overlays**: walkable mask coloring; triangulation/constraint-edge debug view.
4. **Scale toward the full map** (the ~4021-region roadmap): merge each region to a few
   draw calls; glTF/glb (or binary) per-region transport with view-driven streaming + LRU
   eviction; LOD/frustum culling; cull empty/water regions; a 2D minimap for overview.
   Pathfinding is already global server-side (one absolute plane spans all loaded regions),
   so only geometry needs streaming.
5. **Verify the `silkroad_lib` changes on the Linux `bot` build** - especially the
   cross-region link-traversal fix, which changes shared pathfinding behavior.

## Backend entry points (authority: pathfinding.md)

- Parse: `sro::pk2::Pk2ReaderModern{dir}` (loose mode) -> `sro::pk2::NavmeshParser` ->
  `parseNavmesh()` -> `sro::navmesh::triangulation::NavmeshTriangulation`.
- Query: `pathfinder::Pathfinder<NavmeshTriangulation>{tri, cfg}.findShortestPath(S, G)`
  with `cfg = {kPolyanya, agentRadius=3.14, timeout=150ms}`; S/G are absolute `Vector3`.
- Geometry: `Region::tileVertexHeights` (97x97) + `enabledTiles` (96x96);
  `Navmesh::getTransformedObjectResourceForRegion(instanceId, regionId)` for BMS meshes;
  `NavmeshTriangulation::transformRegionPointIntoAbsolute` for the absolute frame.
