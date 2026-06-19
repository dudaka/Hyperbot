# Hyperbot

A toolkit for automating the MMORPG **Silkroad Online**. A man-in-the-middle packet
proxy controls one or more game clients; an event-driven state-machine engine drives
gameplay; and a JAX/Flax reinforcement-learning system trains a combat agent through
self-play PvP. Core is C++20 (CMake + vcpkg); RL math is Python (JAX) embedded via
pybind11; monitoring is a Qt6 desktop app.

## Current focus

A **three.js visualization** of the navmesh (terrain + BMS objects) with interactive
S->G path queries, using Hyperbot's **pathfinding/navigation as the backend**. It is
**implemented and runnable** in `tools/navmesh_viz/` (standalone C++ HTTP service + a
`web/` three.js client). Scopes are concentric `(2R+1)x(2R+1)` rings **1x1..23x23** on a
single center region `5f82`, picked from a dropdown; you can also **load any named zone**.
The default view is a **flat top-down, North-up / East-right map**.

**Several hardening passes are done.** Passes 1-3 (shared `silkroad_lib`, committed):
layer-aware path-height DP, long-segment densification, a cross-region stairway-link fix, the
**object-stitch vertical "teleport"** fix (`0x08` object<->object seams were wrongly usable as
terrain->object on-ramps; only `0x00` object<->terrain should be), a blocked-terrain-walk
guard, and the `0x08` **coincident-seam** direct-crossing fix. Pass 4 (viz-only, committed):
path-timeout labeling, closed/sealed render filter, load-by-zone (226 zones from
`textzonename_*.txt`). The **`Pathfinder` submodule** carries two fixes as an in-repo patch
(`tools/navmesh_viz/patches/pathfinder-polyanya-fixes.patch`, an authorized exception - **not**
a submodule bump, so reapply after `git submodule update`): a Polyanya degeneracy guard
(pass 3) and a `canGetToState` reachability guard (pass 5).

Pass 5 (this session): (a) collapsed the dual center (`5c87`/`5a87`) to a single **`5f82`** and
extended scopes to **23x23 (R=11)** behind a dropdown; (b) the **top-down North-up / East-right
map** (N-S display mirror `world.scale.z=-1` + straight-down camera; picks invert via
`world.worldToLocal` so reported coords stay true); (c) the **reachability guard** -
`canGetToState` re-expanded states thousands of times (no closed-set skip at pop) and timed out
before Polyanya even ran (the real cause of the earlier "object-start times out, terrain-start
is fast" asymmetry); fixed, those queries 5s->~0.5s; (d) proved long cross-mesh paths are
**genuine Polyanya scale** (~3.9M expansions / ~22s for a real ~20-region path, not a bug) and
raised the viz budget **5s->30s**; (e) a **query-time readout** + **Reset view** button.

Current task: **run the Linux `bot` regression** for the shared pathfinding changes (top open
risk - macOS/viz-validated only; reapply the combined Pathfinder patch first); **validate the
coincident-seam threshold** (`kCoincidentSeamEpsilon=8.0`) against real fortress bridges; and
continue GUI path-query testing across the large scopes. Open viz gaps: `5f82` is near the
**north map edge** so large scopes truncate northward; 23x23 geometry is **~222MB** (heavy
browser load; R=11 server ~10-15s startup; a long query blocks the UI ~20s); the closed/sealed
filter's true-positive path is not yet exercised by live data; the zone cache is unbounded; the
429 instanced-dungeon zones are unsupported (codeName region ids, no dungeon navmesh here).

Status, how-to-run, key findings, gaps, and next steps: `docs/threejs-visualization-plan.md`.
Tool usage: `tools/navmesh_viz/README.md`. Backend authority: `docs/pathfinding.md`
(incl. the `0x00`/`0x08` edge semantics). The minimal backend is `silkroad_lib` + the
`Pathfinder` submodule - the full `bot` target is not needed (the tool even compiles only a
subset of `silkroad_lib`).

## Where things live

- `bot/` - the core bot executable (Linux). Packet pipeline, event broker, state
  machines, entity/world model, and the RL training manager.
- `silkroad_lib/` - shared, game-specific library: PK2 game-data parsing, navmesh,
  position math, scalar types. Built on all platforms.
- `ui_proto/` - protobuf message definitions shared between bot and UIs.
- `rl_ui/` - Qt6 monitoring dashboard (Windows/Linux). Connects to the bot over ZeroMQ.
- `client_manager/` + `loader_dll/` - Windows-only. Launch game clients and inject the
  DLL that redirects client TCP connections to the proxy.
- `ui/` - **deprecated legacy UI. Do not modify.**
- `tools/` - Python codegen helpers (`addNewStateMachine.py`, `addNewPacketParser.py`,
  `addNewPacketBuilder.py`).
- `documents/` - assorted historical design notes (unstructured).
- `docs/` - **structured architecture documentation. Start here** (see `docs/README.md`).
  Of note: `docs/pathfinding.md` (navmesh/pathfinding deep dive) and
  `docs/threejs-visualization-plan.md` (the current-focus brief).

## Build

Configure/build via CMake presets (see `CMakePresets.json`). Dependencies come from
vcpkg (`vcpkg.json`) and git submodules in `third_party/` (pybind11, tracy, gli,
pathfinder).

- Bot (Linux): `cmake --preset linux_hyperbot && cmake --build --preset linux_hyperbot`
- RL UI (Linux): `cmake --preset linux_rl_ui && cmake --build --preset linux_rl_ui`
- RL UI (Windows): preset `win64_qt_rl_ui`
- Client manager (Windows 32-bit): preset `win32`

Build is gated by `-DBUILD_BOT`, `-DBUILD_UI`, `-DBUILD_CLIENT_MANAGER` (at least one
required). The bot only builds on Linux (it embeds CPython for JAX). The Python RL env
lives under `bot/src/rl/python/` (`uv`, see its `requirements.txt`); the embedded
interpreter imports `rl.python.dqn`.

## Code style (from AGENTS.md - follow exactly)

- 2-space indent, no tabs. `.hpp`/`.cpp` for headers/sources.
- Private members use a trailing underscore (`foo_`).
- Pointer/ref next to the variable: `Type *var`, `Type &var`.
- Avoid `auto` unless the type is overwhelmingly verbose (container iterators).
- Include order, blank line between each group: (1) local target headers (quoted),
  (2) `silkroad_lib` (angled), (3) third-party except Abseil, (4) Abseil, (5) OS, (6) STL.
- Do not modify `third_party/` or `ui/`.
- No emojis in code, logs, or prints.

## Key architecture facts

- **Data flow:** raw TCP packet -> `PacketProcessor` parses it -> updates `WorldState`
  and publishes an `Event` on the `EventBroker` -> subscribed `StateMachine`s react and
  build/inject packets back through the `Proxy`. See `docs/state-machines.md`.
- **Packets are injected into the *incoming* stream** (`InjectAsReceived`), so the bot's
  own actions flow through the same parse/event pipeline as real traffic.
- **State machines compose** sequentially and concurrently and nest hierarchically
  (`SequentialStateMachines`, `ConcurrentStateMachines`). `Botting` cycles
  `Townlooping` <-> `Training`. Generate new ones with `tools/addNewStateMachine.py`.
- **RL is a DDQN** (Double DQN) with Prioritized Experience Replay, n-step returns, and
  Polyak target updates. Training is self-play: `TrainingManager` pairs characters in
  PvP. The C++ side owns the replay buffer and observation/action storage; Python (JAX)
  owns the network, loss, and optimizer, called via `JaxInterface` (pybind11).
  See `docs/reinforcement-learning.md`.
- **The observation/action spaces are deliberately tiny right now** (compile-time sized
  from `rl/items.hpp` + `rl/skills.hpp`): currently 0 skills + 1 item => observation is
  4 floats, action space is 3 (sleep, common-attack, use-item). Edit those two headers
  to grow them.
- **Game reference data** (items, skills, monsters, levels, navmesh, regions) is parsed
  from the client's encrypted **PK2** archives at startup by `silkroad_lib/.../pk2/`.
- **Silkroad coordinates** are region-relative: a 16-bit `regionId` (packed x/z sectors)
  plus float x/y/z offsets within a 1920-unit region. See `docs/world-model.md`.
- **Navigation is 2.5D**: the world is stacked single-valued surfaces (terrain height field
  + BMS object meshes), not a 3D volume. Pathfinding (external `Pathfinder` lib, Polyanya)
  runs in 2D over X-Z; height (Y) is used only to pick which surface an endpoint sits on.
  Entry point `Bot::calculatePathToDestination` (`bot.cpp`). Deep dive: `docs/pathfinding.md`.
- **Bot <-> rl_ui transport is ZeroMQ:** REQ/REP on tcp `:5555` for commands, PUB/SUB on
  `:5556` for telemetry; payloads are protobuf from `ui_proto/`. See `docs/ui-and-ipc.md`.

## Conventions when extending

- New packet to parse: `tools/addNewPacketParser.py`; new packet to send:
  `tools/addNewPacketBuilder.py`. Both update `bot/CMakeLists.txt`. Register the opcode
  in `bot/src/packet/opcode.hpp`.
- New event type: add to the `EVENT_EVENTCODE_LIST` macro in `event/eventCode.hpp` and a
  struct in `event/event.hpp`.
- New bot behavior: `tools/addNewStateMachine.py`, then wire it into a parent machine.
- The `type_id` system classifies every item/entity via Silkroad's 4-part type id; match
  against `type_id/categories.hpp` rather than hardcoding ref ids.
