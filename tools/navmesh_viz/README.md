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

Region scope is the center region `5c87` (Hotan) plus a ring of radius R
(0 = single region, 1 = 3x3 = 9, 2 = 5x5 = 25). `serve` loads every ring up to R
and caches geometry per radius, so the web UI can switch scope with no reload.

```
# Validate: dump geometry to a file + run a sample path query.
./build/navmesh_viz dump  ../../sro-data/Data 0

# Serve all three scopes (load R=2) on :5577.
./build/navmesh_viz serve ../../sro-data/Data 2 5577
```

Endpoints (CORS-enabled):
- `GET /geometry?r=N` - region set at ring radius N (clamped to the loaded max). Each object
  also carries `outlineEdges` (flat `[srcVertexIndex, destVertexIndex, flag]` triples) for
  edge inspection - flag `0x00` = object<->terrain stitch, `0x08` = object<->object stitch.
- `GET /path?sx&sy&sz&gx&gy&gz` - Polyanya path between two absolute-frame points.
- `GET /info` - `{"maxRadius":N}`, the largest ring the client may request.

## Web client

```
cd web
npm install
npm run dev   # Vite proxies /geometry, /path, /info to 127.0.0.1:5577
```

Open **http://localhost:5173/** (or the next free port Vite prints, e.g. `5174`;
use `localhost`, not `127.0.0.1` - Vite binds IPv6). Scope defaults to **1x1**;
use the 1x1 / 3x3 / 5x5 buttons to switch. Drag to orbit; click to place S then
G - the clicked surface height selects the layer, and the returned path is drawn
following the surfaces. "No path" is expected when the two points are separated
by walls / cliffs / water.

An **on-screen log panel** (and the browser console) records every pick (surface,
absolute coords) and every `/path` request + response - use it to capture the
exact input behind a misbehaving query. "Clear log" empties it.

UI extras:
- **Compass** (top-right) - projects the world axes each frame (`+Z`=North, `+X`=East).
  May read mirror-flipped vs the in-game compass (left-handed Silkroad vs right-handed
  three.js); still a consistent orientation reference.
- **Stitch-edge toggle** (HUD checkbox, off by default) - overlays object outline edges:
  cyan `0x00` (object<->terrain) and magenta `0x08` (object<->object), always-on-top.
- **Debug hooks** (browser console) - `window.__vizQuery(s, g)` runs a path query from
  absolute-coord objects `{x,y,z}`; `window.__vizFocus(target, radius, azDeg, elDeg)` aims
  the camera. Used for automated/visual testing.

See `../../docs/threejs-visualization-plan.md` for status, findings, and gaps, and
`../../docs/pathfinding.md` for the backend authority (incl. the `0x00`/`0x08` semantics).
