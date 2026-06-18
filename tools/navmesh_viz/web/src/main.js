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

// --- Pickable surfaces + markers ---------------------------------------------
const pickables = [];
const raycaster = new THREE.Raycaster();

const markerGroup = new THREE.Group();
world.add(markerGroup);
let pathLine = null;
let endpoints = []; // [{ vec: THREE.Vector3 (absolute), kind: 'S'|'G' }]

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
  for (const mesh of pickables) {
    world.remove(mesh);
    mesh.geometry.dispose();
    mesh.material.dispose();
  }
  pickables.length = 0;
}

async function loadGeometry(radius) {
  currentRadius = radius;
  clearWorld();
  setStatus(`Loading ${['1x1', '3x3', '5x5'][radius]}...`);
  const data = await (await fetch(`/geometry?r=${radius}`)).json();

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

  bbox.getCenter(originOffset);
  world.position.copy(originOffset).multiplyScalar(-1);

  const size = bbox.getSize(new THREE.Vector3());
  const camDist = Math.max(size.x, size.z) * 0.7 + 200;
  camera.position.set(0, camDist, camDist);
  controls.target.set(0, 0, 0);
  controls.update();

  setStatus(`Loaded ${data.regions.length} region(s). Click to place S.`);
}

// --- S/G picking + path query ------------------------------------------------
function addMarker(absoluteVec, kind) {
  const color = kind === 'S' ? 0x5fe08a : 0xff7b7b;
  const marker = new THREE.Mesh(
    new THREE.SphereGeometry(10, 20, 20),
    new THREE.MeshBasicMaterial({ color }));
  marker.position.copy(absoluteVec);
  markerGroup.add(marker);
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
  const url = `/path?sx=${s.x}&sy=${s.y}&sz=${s.z}&gx=${g.x}&gy=${g.y}&gz=${g.z}`;
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

function animate() {
  requestAnimationFrame(animate);
  controls.update();
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

async function init() {
  // Disable scopes the server didn't load (it caps at its startup radius).
  let maxRadius = 2;
  try {
    maxRadius = (await (await fetch('/info')).json()).maxRadius;
  } catch { /* keep default */ }
  for (const btn of scopeButtons) {
    btn.disabled = Number(btn.dataset.radius) > maxRadius;
  }
  await selectScope(Math.min(currentRadius, maxRadius));
}

init();
