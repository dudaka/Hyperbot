# Pathfinding & Navigation (deep dive)

The definitive reference for how Hyperbot represents the world geometrically and finds
paths through it. This is the subsystem the planned **three.js visualization** will use as
its backend, so it is documented in depth. Code lives in `silkroad_lib/.../navmesh/`,
`silkroad_lib/.../pk2/navmeshParser.*`, `bot/src/math/pointTranslator.*`,
`bot/src/bot.cpp` (`calculatePathToDestination`), and `bot/src/state/machine/walking.*`.
The pathfinding algorithm itself is the external **Pathfinder** library
(`third_party/pathfinder`, github.com/SandSnip3r/Pathfinder, a git submodule).

> Companion: [world-model.md](world-model.md) covers entities/coordinates at a higher
> level; this file is the geometry/pathfinding authority.

## 0. The core mental model: it's 2.5D, not 3D

Silkroad's world is **not a volumetric 3D mesh**. It is a set of **single-valued walkable
surfaces stacked in a shared horizontal (X-Z) plane**:

- **Terrain** is a *height field*: exactly one height per (x, z), stored as a per-region
  97x97 vertex grid.
- **Objects (BMS meshes)** — buildings, bridges, multi-floor dungeons — are separate 3D
  meshes placed into regions. These are what create genuinely stacked surfaces at the same
  (x, z) (under vs. on a bridge; floor 1 vs floor 2).

Coordinate convention: **Y is up (height); the ground is the X-Z plane.** Everything below
follows from "search in 2D over X-Z, use Y only to pick which stacked surface an endpoint
is on."

## 1. The 4-stage pipeline

```
PK2 navmesh data  ->  Navmesh  ->  NavmeshTriangulation  ->  Pathfinder (Polyanya)  ->  waypoints
   (tiles, height-     (per-region    (per-region CDT, all     (any-angle 2D shortest   (Walking
    field, edges,       walkable        flattened to one         path; layer carried     state
    object meshes)      geometry)       absolute 2D plane)       in search state)        machine)
```

### Stage 1 - Raw navmesh from PK2 (`navmeshParser.*`, `navmesh.*`)

`NavmeshParser` reads the client's PK2 archives and builds a `Navmesh`. Per `Region`:
96x96 `enabledTiles` (walkable mask), 97x97 `tileVertexHeights` (height field, 20 units
per tile, region = 1920 units), surface flags (water/ice) + `surfaceHeights`, navigation
cells, and edges (`InternalEdge` within a region, `GlobalEdge` on region borders, with
block/bridge/entrance/siege flags). It also loads object meshes: `ObjectResource`
(`vertices: vector<Vector3>`, triangle `cells`, `cellAreaIds`) and `ObjectInstance`
(placement: `center`, `yaw`, `regionId`).

`Region::getHeightAtPoint` (navmesh.cpp:421) does **bilinear interpolation** within a
20-unit tile, with an ice-surface upward override:

```cpp
tileX = x/20; tileZ = z/20; xPercent = (x-20*tileX)/20;
hZ1 = h[tileZ][tileX]   + xPercent*(h[tileZ][tileX+1]   - h[tileZ][tileX]);
hZ2 = h[tileZ+1][tileX] + xPercent*(h[tileZ+1][tileX+1] - h[tileZ+1][tileX]);
height = hZ1 + zPercent*(hZ2 - hZ1);
if (ice cell) height = max(height, surfaceHeights[...]);   // 320-unit climate grid
```

`ObjectResource::getHeight(point, areaId)` (navmesh.cpp:29) finds the mesh triangle in the
given area containing (x,z) and interpolates the height in it; the point is first moved
into the object's local frame, and `objectInstance.center.y` added back.
`getTransformationFromObjectInstanceToWorld` (navmesh.cpp:91) = rotation(-yaw about Y) +
translation(center), plus a region offset when the instance belongs to another region.

### Stage 2 - Triangulation (`navmesh/triangulation/`)

`NavmeshTriangulation` (navmeshTriangulation.cpp:17) makes the searchable map:

- For **each region**, Shewchuk's **Triangle** library (`triangle/triangle_api.h`, vendored
  in Pathfinder) builds a **Constrained Delaunay Triangulation**. Walls/blocked
  terrain/object outlines become *constraint edges*.
- Y is **dropped**: `to2dPoint` keeps `{point.x, point.z}` (navmeshTriangulation.cpp:826);
  vertex dedup compares `.x` and `.z` (line 1047). The triangulation is purely 2D.
- Object triangles are flattened into the same plane but **tagged** with their
  `(objectInstanceId, objectAreaId)` (`markObjectsAndAreasInCells`) so terrain and each
  object floor stay distinguishable.
- Regions are **stitched** into one absolute plane: `linkGlobalEdgesBetweenRegions`
  matches border edges (classified left/right/top/bottom by x or z == 0 or 1920). Triangle
  indices are a combined value packing `(regionId, localTriangleIndex)`.

`NavmeshTriangulation` *is* the pathfinder's map: it inherits
`pathfinder::navmesh::NavmeshTypes<uint32_t>` and implements the accessor API Polyanya
needs (`getSuccessors`, `getTriangleVertices`, `findTriangleForPoint`,
`createStartState`/`createGoalState`, ...). Built once, lives read-only in `GameData`.

### Stage 3 - The pathfinder (Polyanya)

The search is the external Pathfinder library. Hyperbot drives it in
`Bot::calculatePathToDestination` (bot.cpp:720):

```cpp
pathfinder::PathfinderConfig cfg(pathfinder::PathfinderAlgorithm::kPolyanya);
cfg.setAgentRadius(3.14);            // character collision radius -> keeps path off walls
cfg.setTimeout(std::chrono::milliseconds(150));
pathfinder::Pathfinder<NavmeshTriangulation> pf(gameData().navmeshTriangulation(), cfg);
auto result = pf.findShortestPath(navmeshStart /*Vector3*/, navmeshGoal /*Vector3*/);
```

**Polyanya** is an any-angle, optimal pathfinder on polygonal meshes (true Euclidean
shortest path hugging corners, not grid-restricted). Output = path segments
(`StraightPathSegment`s + arcs).

### Stage 4 - Walking the result (`walking.cpp`)

The `Walking` state machine sends one `ClientAgentCharacterMoveRequest` per waypoint, arms
a 333 ms re-request timeout, advances on `EntityMovementBegan/Ended` events, and finishes
at the last waypoint. (Gameplay-side detail; see [state-machines.md](state-machines.md).)

## 2. The height (Y) lifecycle

| Phase | What happens to Y |
|---|---|
| Source (packets) | Parsed from the wire; every entity `Position` carries a real `yOffset`. |
| Gameplay logic | Mostly ignored - distances/ranges use `calculateDistance2d` (X-Z only). |
| Static storage | Terrain = interpolated height field; objects = triangulated meshes w/ per-area heights. |
| Triangulation build | **Dropped** - CDT is 2D; object triangles tagged by `(instance, area)`. |
| Path search | Used **only** in `createStateForPoint` to pick the start/goal surface; then carried in the state. |
| Coord transforms | Passive - `transformRegionPointIntoAbsolute`/`...IntoRegion` shift only X/Z, copy Y through. |
| Output / movement | **Discarded** (waypoints built with y=0); server snaps ground movement to terrain. |

## 3. Endpoints are 3D; the search is 2D

`calculatePathToDestination` takes an `sro::Position` (regionId + x/y/z). Both **S** (self
position) and **G** (destination) are passed as 3D `math::Vector3` into `findShortestPath`
(bot.cpp:820-829). The split:

- **X, Z** -> `findTriangleForPoint` (2D point-in-triangle) picks the footprint/column.
- **Y** -> `createStateForPoint` picks which stacked surface the endpoint sits on.

You **must** supply correct heights for S and G. S is always safe (server-reported self
position); G is only as good as the height the caller passes - a wrong/zero G height can
anchor the goal on the wrong floor.

## 4. Surface selection: which terrain/floor an endpoint sits on

Two-step lookup, with the candidate set precomputed at load:

1. **Horizontal:** `findTriangleForPoint({x,z})` -> the triangle (column of stacked
   surfaces). Height not used here.
2. **Vertical (nearest-altitude wins):** `createStateForPoint(point3d, triangle)`
   (singleRegionNavmeshTriangulation.cpp:692):
   - For each object floor over this triangle (`getObjectDatasForTriangle`), compute its
     height here and track the one with smallest `|objHeight - point.y|`.
   - If terrain is valid here (`!blockedTerrainTriangles_[triangle]`), compute terrain
     height; if `|terrainHeight - point.y|` is strictly smaller, terrain wins.
   - Result state = `setOnTerrain()` or `setObjectData{instance, area}`; throws if neither
     surface exists.

Tie-break: comparisons are strict `<`, objects evaluated first, so an exact tie keeps the
first object and an object beats terrain. `blockedTerrainTriangles_` excludes terrain where
it's sealed under a structure / off a cliff, forcing an object layer.

## 5. Moving between layers mid-path

The A* `State` carries the layer (`onTerrain` or `objectData`), the `entryEdgeIndex` (no
backtracking), and optionally a `linkId_` (mid-transition). Edges carry traversability via
a marker + `ConstraintData` flags (`forTerrain`/`forObject`, `kBlocking`, `kGlobal`,
`kBridge`, `hasLink`/`getLinkId`, `getObjectData`).

`getSuccessors` (singleRegionNavmeshTriangulation.cpp:251) examines the 3 neighbor
triangles; `getSuccessorStateThroughEdgeIfPossible` decides crossing + resulting layer:

- **marker 0** (unconstrained): always pass, keep current layer (the common flat-surface
  case).
- **On an object**, crossing a constraint: blocking -> no successor; external non-bridge ->
  step onto terrain (unless blocked); external bridge -> blocked (can't walk off the deck);
  link edge into the link's triangle set -> **enter link**; other object's constraint ->
  ignored.
- **On terrain**, crossing a constraint: object blocking edge -> no successor; object bridge
  edge -> pass through staying on terrain (**walking under** the bridge); object external
  unblocked edge -> step **onto** the object, unless we're emerging from under it (detected
  by current-triangle-overlaps-object && neighbor-doesn't); terrain blocking -> no
  successor (cliff/wall).
- **Links** = the explicit stairway mechanism: a precomputed triangle patch
  (`linkDataMap_[id].accessibleTriangleIndices`) bridging two objects
  (`globalObjectLinks_[id]`). While traversing, cross any edge staying inside the patch;
  exit on the matching link edge onto the other object.

**The bridge duality** is the proof the 2.5D flattening works: the same physical outline
edge is *blocking* when you're on the bridge and *pass-through* when you're on the terrain
beneath it - disambiguated solely by the state's layer.

## 6. Path post-processing (in `calculatePathToDestination`)

1. Snap the destination to a network-legal point (`NetworkReadyPosition::roundToNearest`);
   return empty if already within `sqrt(0.5)`.
2. Keep only `StraightPathSegment`s; approximate each arc by a single midpoint between
   consecutive straights (bot.cpp:747-757; TODO: can yield a bad path).
3. Prepend the current position; **break up long moves** so each leg stays within one region
   (Δx, Δz <= 1920) - a single move packet can't span more than a region (bot.cpp:771-802).
4. De-duplicate, convert to `NetworkReadyPosition` (height ends up ~0).

## 7. Coordinate systems (cheat-sheet)

- **`sro::Position`**: `regionId` (packed x/z sectors; high bit => dungeon) + float
  `xOffset/yOffset/zOffset`; region = 1920 units; auto-normalizes across borders
  (`position.*`, `position_math.*`).
- **Absolute pathfinder plane**: one 2D plane spanning all regions, centered on
  `originRegionId_ = 16512` (navmeshTriangulation.hpp:61).
  `transformRegionPointIntoAbsolute` / `transformAbsolutePointIntoRegion` convert (X/Z
  shift by 1920*(sector delta), Y copied through).
- **`PointTranslator`** (`bot/src/math/`): a simpler relative-frame helper
  (`sroToPathfinder`/`pathfinderToSro`) used for local 2D math (UI / training area), not the
  main path query.

## 8. Known limitations / TODOs (verified in code)

- **Agent fitting is largely stubbed**: `agentFitsThroughTriangle` returns `true`;
  `agentFitsThroughEdge` only checks edge length >= 2*radius ("LOTS of work to do here").
  Paths may pass through gaps too narrow.
- **Arc -> midpoint approximation** can produce a slightly wrong path.
- **"No return through entry edge"** shortcut breaks the Takla bridge case (commented TODO).
- **Object-overlap link inconsistencies** are handled defensively ("won't work correctly if
  the objects overlap").
- **No-path = throw**: `findShortestPath` empty/throwing propagates as an exception; callers
  must handle "no path."
- **Output waypoints have height ~0** - any 3D consumer must reconstruct surface height.

## 9. Prior art in this repo

The **legacy** `ui/` app already renders the navmesh in 2D (top-down, Qt `QGraphicsScene`):
`ui/navmeshView.*`, `ui/regionGraphicsItem.*`, `ui/map/`. Useful as a reference for which
data is extracted and how regions are drawn - but it is the deprecated UI (do not modify),
and it is 2D, so it cannot show the stacked-surface behavior. The 3D successor now exists
as `tools/navmesh_viz/` (a standalone service over this pathfinder + a three.js client);
see [threejs-visualization-plan.md](threejs-visualization-plan.md).
