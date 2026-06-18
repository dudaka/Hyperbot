# three.js Navmesh Visualization - Project Brief & Status

**Status:** Planning (not started). This document seeds a fresh session whose job is to
**produce a detailed implementation plan** for a three.js demo that visualizes Hyperbot's
navmesh (terrain + BMS objects across multiple regions) and lets the user **interactively
query a path between two points S and G**.

Read [pathfinding.md](pathfinding.md) first - it is the backend authority. This file
records the goal, the relevant findings, the decisions still open, and proposed next steps.

## Goal

A browser app where you can:

1. Load and render **multiple regions**: terrain surfaces + BMS object meshes (buildings,
   bridges, multi-floor structures).
2. Click to place **S** and **G** on specific surfaces (including the correct floor/layer).
3. Query a path S->G using **Hyperbot's pathfinding** and draw the resulting route on the
   surfaces.

## Decision already made: 3D, not 2D

The visualization must be **3D**. Rationale (from the data, see pathfinding.md):

- Terrain is a height field and BMS objects are real 3D meshes - a 2D top-down view
  collapses stacked surfaces (under/over a bridge, floor 1/2) into ambiguous overlaps.
- Interactive S/G picking needs the **Y of the click** to select the layer
  (`createStateForPoint` = nearest-altitude). 2D can't express "the bridge, not the road
  under it" - which is exactly the behavior worth demoing.
- three.js is Y-up, matching Silkroad's Y-up / X-Z-ground convention.

The legacy `ui/navmeshView.*` is prior art but **2D and deprecated**; this is its 3D
successor.

## Backend: what to reuse, what to ignore

**Minimal backend = `silkroad_lib` + the `Pathfinder` submodule.** You do NOT need the full
`bot` target (no Python/JAX, no ZMQ, no packet/proxy stack). `silkroad_lib` already:

- parses PK2 -> `Navmesh` (`pk2/navmeshParser`, `navmesh/navmesh`),
- builds the searchable `NavmeshTriangulation` (`navmesh/triangulation/`),
- and links `Pathfinder` + `gli` (see `silkroad_lib/CMakeLists.txt`).

Key entry points to drive (all in `silkroad_lib` / Pathfinder):

- Build data: `sro::pk2::GameData::parseSilkroadFiles(clientPath)` then
  `gameData.navmeshTriangulation()` (or construct `NavmeshTriangulation(navmesh)` directly).
- Query: `pathfinder::Pathfinder<NavmeshTriangulation>{tri, cfg}.findShortestPath(S, G)`
  with `cfg = {kPolyanya, agentRadius=3.14, timeout=150ms}`; S/G are `math::Vector3` in the
  **absolute** plane (use `transformRegionPointIntoAbsolute`).
- Path -> waypoints: see `Bot::calculatePathToDestination` (bot.cpp:720) for the reference
  post-processing (straight-segments, arc midpoints, long-move splitting). It depends on
  `Bot`/`Self`; for a standalone backend, replicate just the segment-extraction logic, not
  the whole method.

## Render data to extract

- **Terrain mesh** (per region): from `Region` - `tileVertexHeights` (97x97) displaced grid,
  `enabledTiles` (96x96) for walkability coloring/masking; offset each region by
  `(sectorX*1920, 0, sectorZ*1920)` in the absolute frame.
- **BMS object meshes**: per `ObjectInstance`, take its `ObjectResource.vertices`/`cells`,
  apply `getTransformationFromObjectInstanceToWorld` (yaw about Y + center) then the region
  offset. `cellAreaIds` separates floors/areas.
- **(Optional) navmesh triangles / constraint edges** as a debug overlay - from
  `NavmeshTriangulation` accessors (`getTriangleVertices`, edge markers/constraints).
- **Path line**: returned waypoints are 2D (height ~0); **reconstruct height per waypoint**
  for display (terrain `getHeightAtPoint`, or object `getHeight` if that segment is on a
  floor) so the line follows surfaces instead of floating at y=0.

## Interaction

- **S/G picking**: `THREE.Raycaster` against terrain + object meshes; the hit point gives
  (x, z) and the surface Y -> convert to `sro::Position` -> feed as the 3D endpoint. The Y
  is what drives layer selection, so pick against the actual rendered surface.

## Architecture options (for the planning session to choose)

- **Option A - WASM**: compile `silkroad_lib` + `Pathfinder` to WebAssembly (Emscripten);
  run pathfinding in-browser; extract geometry client-side. Pros: no server, fully
  interactive. Cons: heavy Emscripten build of a CMake/abseil/Triangle stack; must ship
  PK2-derived data to the client.
- **Option B - C++ service (recommended first cut)**: a small native binary loads
  `GameData` once and serves (1) region geometry as glTF/JSON and (2) `GET /path?S&G` over
  HTTP/WebSocket; three.js is a thin client. Pros: reuses code as-is, easiest to get
  correct. Cons: needs a host; per-query latency.
- **Option C - offline export + JS reimpl**: export terrain+BMS to glTF and reimplement
  Polyanya + the layer/link logic in JS. Pros: static hosting. Cons: large, high-risk
  reimplementation (the link/bridge logic especially).

**Suggested path:** start with **Option B** on a small region subset to validate geometry
extraction + the query loop, then consider WASM (Option A) as a later optimization if a
serverless/offline demo is wanted.

## Open questions (resolve during planning)

- Which / how many regions to load? The full map is huge - pick a bounded subset (e.g.
  around a town) for the first version; decide a region-streaming strategy later.
- How to obtain PK2-derived data at runtime (need a Silkroad client install path) and how
  to ship it to the browser (Option A/C) vs keep it server-side (Option B)?
- Glb/glTF vs custom JSON for geometry transport; coordinate-frame conventions
  (absolute plane vs per-region) on the wire.
- Picking precision and multi-floor affordances (how the UI lets the user disambiguate
  layers when surfaces overlap in screen space).
- Do we surface debug overlays (triangulation, constraint edges, blocked terrain, links)?

## Risks / gotchas (from backend code; details in pathfinding.md s8)

- `Pathfinder` submodule is currently **not checked out** -> run
  `git submodule update --init --recursive` before any build.
- Agent-fit checks are largely stubbed; arc->midpoint approximation; "no return through
  entry edge" Takla-bridge TODO; object-overlap link inconsistencies; empty path throws.
  The demo should treat the pathfinder as mostly-correct and surface failures gracefully.
- Waypoints have height ~0 by design (server snaps movement) - must reconstruct for 3D.

## First concrete steps (proposed)

1. `git submodule update --init --recursive`; confirm `silkroad_lib` builds standalone with
   the navmesh/triangulation targets (no `bot`).
2. Write a tiny native harness: load `GameData` for a small region set, dump (a) terrain +
   BMS geometry and (b) a sample `findShortestPath` result, to validate the data shapes.
3. Decide Option A/B/C; if B, define the geometry + `/path` API contract.
4. Stand up a minimal three.js scene rendering one region's terrain, then add BMS, then
   raycast picking, then the path query + surface-projected path line.
