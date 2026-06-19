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
(`vertices: vector<Vector3>`, triangle `cells`, `cellAreaIds`, plus `outlineEdges` /
`inlineEdges` as `PrimMeshNavEdge` with a raw `flag`) and `ObjectInstance` (placement:
`center`, `yaw`, `regionId`). **Object outline-edge flags** (`EdgeFlag`: block bits
`0x01`/`0x02`, `kInternal=4`, `kGlobal=8`, `kBridge=16`, `kEntrance=32`, `kSiege=128`)
matter for stitching: an outline edge with flag **`0x00` stitches the object to the
terrain** (a real on-ramp where heights match), whereas **`0x08` stitches it to another
object** (object<->object, traversed via a link only) - conflating the two caused the
terrain->object "teleport" fixed during the navmesh-viz work (§8).

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
  successor (cliff/wall). **Only `0x00` outline edges (object<->terrain stitch) are valid
  step-on ramps; `0x08` outline edges (object<->object stitch) are made blocking from terrain
  and are crossed only object-to-object via their link** - see §8 (the navmesh-viz `0x08`
  fix). Also note: interior **blocked terrain has no constraint edge** (only the per-triangle
  `blockedTerrainTriangles_` flag), so the on-terrain marker-0 pass-through path is now also
  guarded against entering a blocked-terrain triangle.
- **Links** = the explicit object<->object mechanism: a precomputed triangle patch
  (`linkDataMap_[id].accessibleTriangleIndices`) bridging two objects
  (`globalObjectLinks_[id]`). While traversing, cross any edge staying inside the patch;
  exit on the matching link edge onto the other object. **Coincident seams (navmesh-viz
  pass 3):** when a link's two object edges are near-coincident (coplanar floors abutting -
  `LinkData::edgesCoincident`, set in `buildLinkData` when both endpoint gaps are
  `< kCoincidentSeamEpsilon = 8.0`), the patch between them is a degenerate sliver that
  connects one object at only one end. `getSuccessors` then crosses such a seam **directly**
  onto the other object wherever it sits on the far triangle (like a `0x00` on-ramp), so the
  search takes the straight crossing instead of funneling through the corner. Only
  separated-edge links (real bridges) still traverse the corridor. See §8.

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
  the objects overlap"). A **cross-region** link is triangulated inside the single region that
  absorbs both linked objects, so the two objects genuinely overlap on the shared exit
  triangle. `getSuccessors` was updated (during the navmesh-viz work) to pick the link-exit
  object from the *exit edge's own link side* (`isOnSourceSideOfLink`) instead of the old
  "impossible for both objects to be on this triangle" `throw`, and to yield no successor
  (instead of aborting the search) when the link is unknown in the current region. Validated
  via `tools/navmesh_viz` on macOS; **not yet** regression-tested on the Linux `bot`.
- **Object `0x08` outline edges were used as terrain on-ramps (fixed, navmesh-viz work).**
  In `navmeshTriangulation.cpp` both `0x00` and `0x08` outline edges were treated as
  non-blocking "way onto the object", so a search on terrain could step straight up onto a
  raised object floor at an object<->object (`0x08`) seam. Now `0x08` edges get
  `kGlobal | kBlocking` (blocking from terrain; the object<->object link still works because
  link handling runs before the blocking check on the object side); only `0x00` edges stitch
  terrain->object. Caveat: confirm this does not over-block object<->object connections whose
  link data is missing (the "link data does not actually exist" branch). macOS/viz-validated;
  **not yet** Linux `bot`-tested.
- **On-terrain walks could cross blocked terrain (fixed, navmesh-viz work).** Interior blocked
  terrain has no constraint edge (only `blockedTerrainTriangles_`), and `getSuccessors` checked
  that flag only when leaving an object / at start+goal. A guard now drops any successor that
  stays on terrain but enters a blocked-terrain triangle. macOS/viz-validated; **not yet**
  Linux `bot`-tested.
- **Object<->object `0x08` coincident seams crossed directly (fixed, navmesh-viz pass 3).**
  When two coplanar object floors abut, their `0x08` link's two edges are near-coincident and
  the triangulated link patch between them is a degenerate sliver that connects one object at
  only one end - so a search funnels through that corner (a ~60-unit detour) and, in the dense
  geometry, can trip a Polyanya numeric degeneracy that aborts the search. `buildLinkData` now
  flags `edgesCoincident` (endpoint gap `< kCoincidentSeamEpsilon = 8.0`), and `getSuccessors`
  crosses a coincident seam directly object-to-object (like a `0x00` on-ramp), bypassing the
  corridor; separated-edge links keep the corridor. **Heuristic risk:** a genuine bridge whose
  facing edges sit < 8 units apart would be misclassified - validate against the bandit/jangan
  fortress bridges. Underlying cause unfixed: `accessibleTriangleIndices` omits tiny triangles
  below the `trianglesOverlap` epsilon (the "Big TODO" in `buildLinkData`). macOS/viz-validated;
  **not yet** Linux `bot`-tested.
- **Polyanya "i1/i2 is inf" degeneracy guarded in the vendored lib (navmesh-viz pass 3).** The
  interval-projection "parallel edges closer than the agent radius" case in
  `third_party/Pathfinder/pathfinder.h` (`doesRightIntervalIntersectWithLeftOfSuccessorEdge`
  and its mirror) **threw**, aborting the whole search - masked as "No path" under the 150ms
  timeout. The two throw sites now fall back to the library's own "skip this successor unless we
  can turn around the constraint" logic (regression-safe: only previously-crashing inputs
  change). This **modifies `third_party/`** - an authorized exception to the usual rule - kept
  as `tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch` (which also bundles the
  reachability guard below), **not** a submodule bump (a fresh `git submodule update` reverts
  it). Now optional: the coincident-seam fix avoids the degenerate corridor, so the case is no
  longer reached for known data.
- **`canGetToState` reachability pre-check re-expansion blow-up guarded (navmesh-viz pass 5,
  in the same patch).** Before Polyanya runs, `findShortestPath` calls `canGetToState` - a
  greedy best-first triangle search that returns whether the goal is reachable at all. It
  enqueued successors already on the heap and had **no closed-set skip at pop**, so on dense /
  large meshes the same states were re-expanded thousands of times (observed ~20M expansions
  over only ~thousands of distinct states, heap into the millions) and the pre-check **timed
  out -> empty result -> reported as "no path"/"timeout"**. This was the real cause of the
  earlier asymmetric "starts-on-an-object-surface times out, starts-on-terrain is fast" puzzle.
  Fix: a one-line `if (visited.find(currentState) != visited.end()) continue;` at pop. Effect:
  those object-start queries drop from a 5s timeout to ~0.5s; each state is expanded once.
  Regression-safe (reachability is boolean; skipping an already-expanded state changes nothing).
- **No-path = throw OR empty**: `findShortestPath` either throws (bad start/goal triangle,
  degeneracy) or returns an **empty** path. A **timeout** returns empty too (its reachability
  pre-check `canGetToState` returns `false` on the deadline), so an empty result is
  **indistinguishable from a genuine disconnect** unless you time the call. The `bot` uses
  150ms (above); the `navmesh_viz` tool raised its own copy to **30s** and times the call to
  report `"Search timed out"` vs `"No path found"` (see threejs-visualization-plan.md, pass 5).
- **Polyanya cost is genuinely O(millions of expansions) for very long paths.** A path that
  crosses ~20 regions of finely-triangulated terrain can need ~3.9M interval expansions and
  ~22s - and yet be a real, optimal path (measured on the 23x23 viz scope; the reachability
  pre-check confirmed connectivity in ~2k expansions, and Polyanya's `expansions == visited`,
  so there is no re-expansion bug - it is inherent scale). This is why the viz budget is 30s;
  it is distinct from the `canGetToState` bug above (that was re-expansion; this is distance).
- **Output waypoints have height ~0** - any 3D consumer must reconstruct surface height. The
  Pathfinder result (`StraightPathSegment`) exposes only 2D points; the A* surface/layer state
  is internal and not returned, so a 3D consumer must re-derive which surface each leg is on.

## 9. Prior art in this repo

The **legacy** `ui/` app already renders the navmesh in 2D (top-down, Qt `QGraphicsScene`):
`ui/navmeshView.*`, `ui/regionGraphicsItem.*`, `ui/map/`. Useful as a reference for which
data is extracted and how regions are drawn - but it is the deprecated UI (do not modify),
and it is 2D, so it cannot show the stacked-surface behavior. The 3D successor now exists
as `tools/navmesh_viz/` (a standalone service over this pathfinder + a three.js client);
see [threejs-visualization-plan.md](threejs-visualization-plan.md).
