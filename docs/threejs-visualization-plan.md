# three.js Navmesh Visualization - Status & Handoff

**Status: first cut IMPLEMENTED and verified on macOS.** Region scopes 1x1 / 3x3 / 5x5
(center region `5c87` = Hotan, plus a ring) render in 3D; interactive S->G path queries
work against Hyperbot's own pathfinder. The code lives in **`tools/navmesh_viz/`**
(standalone C++ backend + `web/` three.js client). This file is the handoff for the next
session, whose job is to **test the web UI and the GUI path queries** and then push the
scaling/quality work in "Next steps".

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
  query + per-waypoint height reconstruction. `server.cpp` - cpp-httplib endpoints.
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

Open **<http://localhost:5173/>** (use `localhost`, not `127.0.0.1` - Vite binds IPv6).
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

## Known gaps / issues

- **Path height reconstruction is terrain-only.** Waypoints on an object floor fall back to
  terrain/start height (`path.cpp` `reconstructHeight`), so multi-floor routes don't yet
  hug the floor. Needs layer-aware reconstruction (carry the surface, or use
  nearest-altitude continuity from S).
- **"No path" is often correct, not a bug** - two picks separated by walls/cliffs/water
  legitimately have no route. Some Pathfinder edge cases also return clean errors
  ("impossible for both objects to be on this triangle", "invalid point"); these are
  surfaced, not crashes.
- **Walkable mask** (`terrain.walkable`, 96x96 per region) is sent but not rendered as an
  overlay yet.
- **Debug overlays** (triangulation triangles / constraint edges) are not implemented.
- **Per-object draw calls don't scale**: ~250 meshes at R=2 (~31 FPS headless, 12.5 MB
  geometry). Fine for 1x1/3x3/5x5; not for the full ~4021-region map.
- The `silkroad_lib` changes (loose reader, allow-list) are additive and compile on macOS
  via `navmesh_viz`, but were **not** built against the Linux `bot` target - verify there
  before relying on the bot.

## Next steps

1. **Test the UI/GUI queries thoroughly** (current task): scope switching, picking on
   stacked surfaces (walls/structures vs ground), cross-region paths, and failure messaging.
2. **Layer-aware path height** so multi-floor routes follow the correct surface.
3. **Optional overlays**: walkable mask coloring; triangulation/constraint-edge debug view.
4. **Scale toward the full map** (the ~4021-region roadmap): merge each region to a few
   draw calls; glTF/glb (or binary) per-region transport with view-driven streaming + LRU
   eviction; LOD/frustum culling; cull empty/water regions; a 2D minimap for overview.
   Pathfinding is already global server-side (one absolute plane spans all loaded regions),
   so only geometry needs streaming.
5. Verify the `silkroad_lib` changes on the Linux `bot` build.

## Backend entry points (authority: pathfinding.md)

- Parse: `sro::pk2::Pk2ReaderModern{dir}` (loose mode) -> `sro::pk2::NavmeshParser` ->
  `parseNavmesh()` -> `sro::navmesh::triangulation::NavmeshTriangulation`.
- Query: `pathfinder::Pathfinder<NavmeshTriangulation>{tri, cfg}.findShortestPath(S, G)`
  with `cfg = {kPolyanya, agentRadius=3.14, timeout=150ms}`; S/G are absolute `Vector3`.
- Geometry: `Region::tileVertexHeights` (97x97) + `enabledTiles` (96x96);
  `Navmesh::getTransformedObjectResourceForRegion(instanceId, regionId)` for BMS meshes;
  `NavmeshTriangulation::transformRegionPointIntoAbsolute` for the absolute frame.
