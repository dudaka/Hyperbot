# navmesh_viz

3D three.js visualization of Hyperbot's navmesh (terrain + BMS objects) with
interactive S->G path queries, backed by `silkroad_lib` + the `Pathfinder`
submodule. See `docs/threejs-visualization-plan.md` and `docs/pathfinding.md`.

The backend is a standalone native binary (no `bot`, Qt, vcpkg, or protobuf). It
reads the already-extracted `sro-data/` loose files and serves geometry + path
queries over HTTP; the `web/` client renders them.

## Build (macOS / Linux)

Prerequisites: `git submodule update --init --recursive` (Pathfinder, gli),
abseil and cpp-httplib available (e.g. `brew install abseil cpp-httplib`).

```
cd tools/navmesh_viz
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build --target navmesh_viz
```

## Run

Region scope is the center region `5f82` plus a ring of radius R: scope is the
(2R+1)x(2R+1) square of regions, for R = 0 (1x1) up to R = 11 (23x23 = 529). Every
scope is concentric on `5f82`. `serve` loads every ring up to R and caches geometry
per radius, so the web UI can switch scope with no reload. Regions that are
non-existent, closed (no walkable area), or sealed off (no usable link to a neighbor
in view) are omitted from rendering.

```
# Validate: dump geometry to a file + run a sample path query.
./build/navmesh_viz dump  ../../sro-data/Data 0

# Serve all scopes up to 23x23 (load R=11) on :5577.
./build/navmesh_viz serve ../../sro-data/Data 11 5577
```

Endpoints (CORS-enabled):
- `GET /geometry?r=N` - region set at ring radius N (clamped to the loaded max). Each object
  also carries `outlineEdges` (flat `[srcVertexIndex, destVertexIndex, flag]` triples) for
  edge inspection - flag `0x00` = object<->terrain stitch, `0x08` = object<->object stitch.
- `GET /geometry?zone=NAME` - the regions of a named zone, built on demand (a separate
  navmesh + triangulation per zone) and cached. Same closed/sealed/non-existent render
  filter as the radius scopes.
- `GET /zones` - `[{"name":..,"regions":N}, ...]`, the loadable zones sorted by name.
- `GET /path?sx&sy&sz&gx&gy&gz[&zone=NAME]` - Polyanya path between two absolute-frame
  points; with `zone` the query runs within that zone's triangulation, otherwise the radius one.
- `GET /info` - `{"maxRadius":N,"agentRadius":R}`: the largest ring the client may request,
  and the pathfinder's agent (collision) radius so the client can draw the footprint to scale.

Zones come from the Silkroad `textzonename_*.txt` files (under
`<dataPath>/../Media/server_dep/silkroad/textdata`): a zone is the set of regions sharing
one English name (field index 9), keyed by the numeric region id (field index 2). Rows whose
region-id column is a symbolic codeName (instanced dungeons, which have no overworld navmesh)
are skipped, so only loadable zones are listed.

## Web client

```
cd web
npm install
npm run dev   # Vite proxies /geometry, /path, /info to 127.0.0.1:5577
```

Open **http://localhost:5173/** (or the next free port Vite prints, e.g. `5174`;
use `localhost`, not `127.0.0.1` - Vite binds IPv6). Scope defaults to **1x1**;
pick a scope from the dropdown to switch. The default camera is a **flat top-down,
North-up / East-right map** (conventional orientation - the display mirrors the
North-South axis to make both hold at once, since Silkroad is left-handed and
three.js right-handed). Drag to orbit; click to place S then G - the clicked
surface height selects the layer, and the returned path is drawn following the
surfaces. "No path" is expected when the two points are separated by walls /
cliffs / water.

An **on-screen log panel** (and the browser console) records every pick (surface,
absolute coords) and every `/path` request + response - use it to capture the
exact input behind a misbehaving query. "Clear log" empties it.

The path result shows the **query time** (e.g. `Path: N waypoint(s) in 21ms`); a long
cross-mesh query on a big scope can take ~20s (the backend Polyanya budget is 30s), and the
UI blocks until it returns. **Reset view** re-frames to the default top-down in one click
(distinct from **Reset S/G**, which only clears the markers + path).

UI extras:
- **Compass** (top-right) - projects the world axes each frame. With the North-South
  display mirror, North (`+Z`) reads up and East (`+X`) right, matching a conventional
  map; the rose stays correct through any orbit.
- **Stitch-edge toggle** (HUD checkbox, off by default) - overlays object outline edges:
  cyan `0x00` (object<->terrain) and magenta `0x08` (object<->object), always-on-top.
- **Agent-radius rings** - each S/G marker draws a flat ring of radius = the backend's agent
  radius (from `/info`), so the character collision footprint is visible to scale.
- **Debug hooks** (browser console) - `window.__vizQuery(s, g)` runs a path query from
  absolute-coord objects `{x,y,z}`; `window.__vizFocus(target, radius, azDeg, elDeg)` aims
  the camera. Used for automated/visual testing.

See `../../docs/threejs-visualization-plan.md` for status, findings, and gaps, and
`../../docs/pathfinding.md` for the backend authority (incl. the `0x00`/`0x08` semantics).
