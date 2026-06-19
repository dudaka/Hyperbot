import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

const statusEl = document.getElementById('status');
const setStatus = (text) => { statusEl.textContent = text; };

// Mirror an interaction to the console and the on-screen log panel.
const logEl = document.getElementById('log');
function logLine(cls, label, data) {
  console.log(label, data);
  const entry = document.createElement('div');
  entry.className = `entry ${cls}`;
  entry.textContent = `${label} ${JSON.stringify(data)}`;
  logEl.prepend(entry);
}
document.getElementById('clearLog').addEventListener('click', () => {
  logEl.replaceChildren();
});

// --- Renderer / scene / camera ------------------------------------------------
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(window.innerWidth, window.innerHeight);
document.getElementById('app').appendChild(renderer.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x0b0e14);

const camera = new THREE.PerspectiveCamera(
  55, window.innerWidth / window.innerHeight, 1, 100000);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;

scene.add(new THREE.HemisphereLight(0xcfe3ff, 0x202830, 1.1));
const sun = new THREE.DirectionalLight(0xffffff, 1.4);
sun.position.set(0.5, 1, 0.3);
scene.add(sun);

// Everything is stored in absolute coords; the group is shifted so the loaded
// region sits near the origin (better float precision + simpler camera).
const world = new THREE.Group();
scene.add(world);
let originOffset = new THREE.Vector3();
let currentRadius = 0;
// Name of the loaded zone, or null when a radius scope is shown. When set, path
// queries run within that zone's triangulation (passed as &zone=).
let activeZone = null;

// --- Pickable surfaces + markers ---------------------------------------------
const pickables = [];
const raycaster = new THREE.Raycaster();

const markerGroup = new THREE.Group();
world.add(markerGroup);
let pathLine = null;
let endpoints = []; // [{ vec: THREE.Vector3 (absolute), kind: 'S'|'G' }]

// Overlay for object outline "stitch" edges: 0x00 = object<->terrain stitch,
// 0x08 = object<->object stitch. Hidden by default; toggled from the HUD.
const stitchGroup = new THREE.Group();
stitchGroup.visible = false;
world.add(stitchGroup);
const showStitch = document.getElementById('showStitch');

function clearStitch() {
  for (const lines of stitchGroup.children) {
    lines.geometry.dispose();
    lines.material.dispose();
  }
  stitchGroup.clear();
}

// Builds always-on-top line overlays for the 0x00 and 0x08 outline edges so they
// can be located even behind structures.
function buildStitchEdges(regions) {
  const byFlag = { 0: [], 8: [] }; // 0x00, 0x08 -> flat [x,y,z,...] segment pairs
  const lift = 1.5; // raise off the surface a touch
  for (const region of regions) {
    for (const object of region.objects) {
      const P = object.positions;
      const E = object.outlineEdges || [];
      for (let i = 0; i < E.length; i += 3) {
        const arr = byFlag[E[i + 2]];
        if (!arr) continue; // only 0x00 and 0x08
        const s = E[i] * 3, d = E[i + 1] * 3;
        arr.push(P[s], P[s + 1] + lift, P[s + 2], P[d], P[d + 1] + lift, P[d + 2]);
      }
    }
  }
  const colors = { 0: 0x00e5ff, 8: 0xff6ad5 };
  for (const flag of [0, 8]) {
    const pts = byFlag[flag];
    if (pts.length === 0) continue;
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(pts), 3));
    const material = new THREE.LineBasicMaterial({
      color: colors[flag], depthTest: false, depthWrite: false });
    const lines = new THREE.LineSegments(geometry, material);
    lines.renderOrder = 999; // draw on top of surfaces
    stitchGroup.add(lines);
  }
}

showStitch.addEventListener('change', () => {
  stitchGroup.visible = showStitch.checked;
});

function colorForArea(areaId) {
  // Distinct hue per object area/floor so stacked surfaces are distinguishable.
  const hue = (areaId * 0.18 + 0.08) % 1.0;
  return new THREE.Color().setHSL(hue, 0.55, 0.58);
}

function makeTerrainMesh(terrain) {
  const geometry = new THREE.BufferGeometry();
  const positions = new Float32Array(terrain.positions);
  geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geometry.setIndex(terrain.indices);
  geometry.computeVertexNormals();

  // Height-gradient vertex colors (low = green valley, high = pale ridge).
  let minY = Infinity, maxY = -Infinity;
  for (let i = 1; i < positions.length; i += 3) {
    minY = Math.min(minY, positions[i]);
    maxY = Math.max(maxY, positions[i]);
  }
  const span = Math.max(1, maxY - minY);
  const colors = new Float32Array(positions.length);
  const low = new THREE.Color(0x2f6b3a);
  const high = new THREE.Color(0xb9c4a0);
  const c = new THREE.Color();
  for (let v = 0; v < positions.length / 3; v++) {
    const t = (positions[v * 3 + 1] - minY) / span;
    c.copy(low).lerp(high, t);
    colors[v * 3] = c.r; colors[v * 3 + 1] = c.g; colors[v * 3 + 2] = c.b;
  }
  geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));

  const material = new THREE.MeshStandardMaterial({
    vertexColors: true, side: THREE.DoubleSide, roughness: 0.95, metalness: 0.0,
  });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.userData.kind = 'terrain';
  return mesh;
}

function makeObjectMesh(object) {
  const geometry = new THREE.BufferGeometry();
  const positions = new Float32Array(object.positions);
  geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geometry.setIndex(object.indices);

  // Per-triangle area color: expand to per-vertex via the index buffer.
  const colors = new Float32Array(positions.length);
  for (let t = 0; t < object.indices.length; t += 3) {
    const col = colorForArea(object.areaIds[t / 3] ?? 0);
    for (let k = 0; k < 3; k++) {
      const vi = object.indices[t + k];
      colors[vi * 3] = col.r; colors[vi * 3 + 1] = col.g; colors[vi * 3 + 2] = col.b;
    }
  }
  geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
  geometry.computeVertexNormals();

  const material = new THREE.MeshStandardMaterial({
    vertexColors: true, side: THREE.DoubleSide, roughness: 0.7, metalness: 0.05,
  });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.userData.kind = 'object';
  return mesh;
}

// Removes and frees all loaded surface meshes (terrain + objects) and any
// active selection, so a fresh region set can be loaded into the same group.
function clearWorld() {
  clearSelection();
  clearStitch();
  for (const mesh of pickables) {
    world.remove(mesh);
    mesh.geometry.dispose();
    mesh.material.dispose();
  }
  pickables.length = 0;
}

// Fetches a geometry document (radius or zone) and renders it, reframing the
// camera. `loadedSuffix` is appended to the "Loaded N region(s)" status.
async function renderGeometry(url, loadingLabel, loadedSuffix = '') {
  clearWorld();
  setStatus(loadingLabel);
  const data = await (await fetch(url)).json();
  if (data.error) {
    setStatus(`Error: ${data.error}`);
    return;
  }
  if (!data.regions || data.regions.length === 0) {
    setStatus(`No renderable regions${loadedSuffix}.`);
    return;
  }

  const bbox = new THREE.Box3();
  const tmp = new THREE.Vector3();
  const addPositions = (arr) => {
    for (let i = 0; i < arr.length; i += 3) {
      bbox.expandByPoint(tmp.set(arr[i], arr[i + 1], arr[i + 2]));
    }
  };

  for (const region of data.regions) {
    const terrain = makeTerrainMesh(region.terrain);
    world.add(terrain);
    pickables.push(terrain);
    addPositions(region.terrain.positions);
    for (const object of region.objects) {
      const mesh = makeObjectMesh(object);
      world.add(mesh);
      pickables.push(mesh);
      addPositions(object.positions);
    }
  }

  buildStitchEdges(data.regions);
  stitchGroup.visible = showStitch.checked;

  bbox.getCenter(originOffset);
  world.position.copy(originOffset).multiplyScalar(-1);

  const size = bbox.getSize(new THREE.Vector3());
  const camDist = Math.max(size.x, size.z) * 0.7 + 200;
  camera.position.set(0, camDist, camDist);
  controls.target.set(0, 0, 0);
  controls.update();

  setStatus(`Loaded ${data.regions.length} region(s)${loadedSuffix}. Click to place S.`);
}

async function loadGeometry(radius) {
  currentRadius = radius;
  await renderGeometry(`/geometry?r=${radius}`,
    `Loading ${['1x1', '3x3', '5x5', '7x7', '9x9'][radius]}...`);
}

async function loadZone(name) {
  await renderGeometry(`/geometry?zone=${encodeURIComponent(name)}`,
    `Loading zone ${name}...`, ` - zone: ${name}`);
}

// --- S/G picking + path query ------------------------------------------------
// Character collision radius used by the pathfinder backend, reported by the
// server (GET /info). Drawn as a flat ring at each endpoint so its scale is
// visible against the navmesh. The 3.14 default is only a fallback until /info
// loads.
let agentRadius = 3.14;

function agentRadiusRing(absoluteVec, color) {
  const segments = 64;
  const pts = [];
  for (let i = 0; i <= segments; i++) {
    const a = (i / segments) * Math.PI * 2;
    pts.push(new THREE.Vector3(Math.cos(a) * agentRadius, 0, Math.sin(a) * agentRadius));
  }
  const ring = new THREE.Line(
    new THREE.BufferGeometry().setFromPoints(pts),
    new THREE.LineBasicMaterial({ color }));
  ring.position.copy(absoluteVec);
  ring.renderOrder = 999;
  ring.material.depthTest = false;
  return ring;
}

function addMarker(absoluteVec, kind) {
  const color = kind === 'S' ? 0x5fe08a : 0xff7b7b;
  const marker = new THREE.Mesh(
    new THREE.SphereGeometry(10, 20, 20),
    new THREE.MeshBasicMaterial({ color }));
  marker.position.copy(absoluteVec);
  markerGroup.add(marker);
  markerGroup.add(agentRadiusRing(absoluteVec, color));
}

function clearSelection() {
  endpoints = [];
  markerGroup.clear();
  if (pathLine) { world.remove(pathLine); pathLine.geometry.dispose(); pathLine = null; }
  setStatus('Cleared. Click to place S.');
}

async function queryPath() {
  const [s, g] = endpoints.map((e) => e.vec);
  setStatus('Querying path...');
  let url = `/path?sx=${s.x}&sy=${s.y}&sz=${s.z}&gx=${g.x}&gy=${g.y}&gz=${g.z}`;
  if (activeZone) {
    url += `&zone=${encodeURIComponent(activeZone)}`;
  }
  logLine('req', '[path] request', {
    S: { x: s.x, y: s.y, z: s.z },
    G: { x: g.x, y: g.y, z: g.z },
    url,
  });
  const res = await (await fetch(url)).json();
  logLine(res.ok ? 'res-ok' : 'res-err', '[path] response', res);
  if (!res.ok) {
    setStatus(`No path: ${res.error}`);
    return;
  }
  const points = res.waypoints.map((w) => new THREE.Vector3(w[0], w[1] + 3, w[2]));
  // A tube reads far better than a 1px line at this scale.
  const curve = new THREE.CatmullRomCurve3(points);
  const geometry = new THREE.TubeGeometry(
    curve, Math.max(2, points.length * 8), 3.5, 8, false);
  pathLine = new THREE.Mesh(
    geometry, new THREE.MeshBasicMaterial({ color: 0xffe14d }));
  world.add(pathLine);
  setStatus(`Path: ${points.length} waypoint(s). Click Reset to retry.`);
}

function onPick(clientX, clientY) {
  const ndc = new THREE.Vector2(
    (clientX / window.innerWidth) * 2 - 1,
    -(clientY / window.innerHeight) * 2 + 1);
  raycaster.setFromCamera(ndc, camera);
  const hits = raycaster.intersectObjects(pickables, false);
  if (hits.length === 0) return;

  // Hit point is in world space; convert back to the absolute frame.
  const hit = hits[0];
  const absolute = hit.point.clone().add(originOffset);
  if (endpoints.length >= 2) clearSelection();
  const kind = endpoints.length === 0 ? 'S' : 'G';
  endpoints.push({ vec: absolute, kind });
  addMarker(absolute, kind);
  logLine('pick', `[pick] ${kind}`, {
    surface: hit.object.userData.kind,
    absolute: { x: absolute.x, y: absolute.y, z: absolute.z },
    worldPoint: { x: hit.point.x, y: hit.point.y, z: hit.point.z },
    ndc: { x: ndc.x, y: ndc.y },
  });
  setStatus(`${kind} = (${absolute.x.toFixed(0)}, ${absolute.y.toFixed(0)}, ` +
            `${absolute.z.toFixed(0)})`);
  if (endpoints.length === 2) queryPath();
}

// Treat a click as a pick only if the pointer barely moved (so orbit drags
// don't place markers).
let downPos = null;
renderer.domElement.addEventListener('pointerdown', (e) => {
  downPos = { x: e.clientX, y: e.clientY };
});
renderer.domElement.addEventListener('pointerup', (e) => {
  if (!downPos) return;
  const moved = Math.hypot(e.clientX - downPos.x, e.clientY - downPos.y);
  downPos = null;
  if (moved < 5) onPick(e.clientX, e.clientY);
});
document.getElementById('reset').addEventListener('click', clearSelection);

window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});

// --- Compass ------------------------------------------------------------------
// World +Z is North and +X is East (Silkroad region axes). Each frame the world
// axes are projected to screen so the rose stays correct through any rotation.
const compass = document.getElementById('compass');
const compassCtx = compass.getContext('2d');
const compassSize = 96;
{
  const dpr = window.devicePixelRatio || 1;
  compass.width = compassSize * dpr;
  compass.height = compassSize * dpr;
  compassCtx.scale(dpr, dpr);
}
const compassDirs = [
  { label: 'N', v: [0, 0, 1], accent: true },
  { label: 'E', v: [1, 0, 0] },
  { label: 'S', v: [0, 0, -1] },
  { label: 'W', v: [-1, 0, 0] },
];
const _ca = new THREE.Vector3();
const _cb = new THREE.Vector3();
function screenDir(x, y, z) {
  _ca.copy(controls.target).project(camera);
  _cb.set(x, y, z).multiplyScalar(200).add(controls.target).project(camera);
  const dx = _cb.x - _ca.x, dy = _cb.y - _ca.y;
  const len = Math.hypot(dx, dy) || 1;
  return [dx / len, dy / len];
}
function drawCompass() {
  const s = compassSize, c = s / 2, r = s / 2 - 18;
  compassCtx.clearRect(0, 0, s, s);
  compassCtx.beginPath();
  compassCtx.arc(c, c, r + 10, 0, Math.PI * 2);
  compassCtx.fillStyle = 'rgba(16, 20, 28, 0.82)';
  compassCtx.fill();
  compassCtx.strokeStyle = '#2a3342';
  compassCtx.stroke();
  compassCtx.font = 'bold 12px ui-monospace, Menlo, monospace';
  compassCtx.textAlign = 'center';
  compassCtx.textBaseline = 'middle';
  for (const d of compassDirs) {
    const [sx, sy] = screenDir(d.v[0], d.v[1], d.v[2]);
    const px = c + sx * r, py = c - sy * r; // canvas y is down; NDC y is up
    compassCtx.beginPath();
    compassCtx.moveTo(c, c);
    compassCtx.lineTo(px, py);
    compassCtx.strokeStyle = d.accent ? '#ff7b7b' : '#46536b';
    compassCtx.lineWidth = d.accent ? 2 : 1;
    compassCtx.stroke();
    compassCtx.fillStyle = d.accent ? '#ff7b7b' : '#9aa6b8';
    compassCtx.fillText(d.label, c + sx * (r + 9), c - sy * (r + 9));
  }
}

function animate() {
  requestAnimationFrame(animate);
  controls.update();
  drawCompass();
  renderer.render(scene, camera);
}
animate();

// --- Region-scope switcher ---------------------------------------------------
const scopeButtons = [...document.querySelectorAll('#scope button')];

function setActiveScope(radius) {
  for (const btn of scopeButtons) {
    btn.classList.toggle('active', Number(btn.dataset.radius) === radius);
  }
}

async function selectScope(radius) {
  activeZone = null; // back to radius mode
  setActiveScope(radius);
  try {
    await loadGeometry(radius);
  } catch (err) {
    setStatus(`Error: ${err.message}`);
  }
}

for (const btn of scopeButtons) {
  btn.addEventListener('click', () => selectScope(Number(btn.dataset.radius)));
}

// --- Zone loader -------------------------------------------------------------
const zoneInput = document.getElementById('zoneInput');
const zoneList = document.getElementById('zoneList');

async function selectZone(name) {
  if (!name) return;
  activeZone = name;
  setActiveScope(-1); // no radius scope is active in zone mode
  try {
    await loadZone(name);
  } catch (err) {
    setStatus(`Error: ${err.message}`);
  }
}

document.getElementById('loadZone').addEventListener('click',
  () => selectZone(zoneInput.value.trim()));
zoneInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') selectZone(zoneInput.value.trim());
});

async function populateZones() {
  try {
    const zones = await (await fetch('/zones')).json();
    zoneList.replaceChildren();
    for (const z of zones) {
      const opt = document.createElement('option');
      opt.value = z.name;
      opt.label = `${z.regions} region(s)`;
      zoneList.appendChild(opt);
    }
  } catch { /* zone picker stays empty */ }
}

async function init() {
  // Disable scopes the server didn't load (it caps at its startup radius).
  let maxRadius = 2;
  try {
    const info = await (await fetch('/info')).json();
    maxRadius = info.maxRadius;
    if (typeof info.agentRadius === 'number') agentRadius = info.agentRadius;
  } catch { /* keep defaults */ }
  for (const btn of scopeButtons) {
    btn.disabled = Number(btn.dataset.radius) > maxRadius;
  }
  await populateZones();
  await selectScope(Math.min(currentRadius, maxRadius));
}

init();

// Debug hook for automated/visual testing: place S and G from absolute coords
// and run the path query, bypassing mouse picking.
window.__vizQuery = (s, g) => {
  clearSelection();
  for (const [p, kind] of [[s, 'S'], [g, 'G']]) {
    const vec = new THREE.Vector3(p.x, p.y, p.z);
    endpoints.push({ vec, kind });
    addMarker(vec, kind);
  }
  return queryPath();
};

// Debug hook: aim the camera at an absolute-frame point from a given distance
// and azimuth/elevation (degrees), for framing screenshots.
window.__vizFocus = (target, radius = 400, azDeg = 45, elDeg = 30) => {
  const az = azDeg * Math.PI / 180, el = elDeg * Math.PI / 180;
  const t = new THREE.Vector3(target.x, target.y, target.z).sub(originOffset);
  controls.target.copy(t);
  camera.position.set(
    t.x + radius * Math.cos(el) * Math.sin(az),
    t.y + radius * Math.sin(el),
    t.z + radius * Math.cos(el) * Math.cos(az));
  controls.update();
};
