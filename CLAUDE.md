# Hyperbot

A toolkit for automating the MMORPG **Silkroad Online**. A man-in-the-middle packet
proxy controls one or more game clients; an event-driven state-machine engine drives
gameplay; and a JAX/Flax reinforcement-learning system trains a combat agent through
self-play PvP. Core is C++20 (CMake + vcpkg); RL math is Python (JAX) embedded via
pybind11; monitoring is a Qt6 desktop app.

## Current focus

A **three.js visualization** of the navmesh (terrain + BMS objects) with interactive
S->G path queries, using Hyperbot's **pathfinding/navigation as the backend**. The first
cut is **implemented and runnable** in `tools/navmesh_viz/` (standalone C++ HTTP service +
a `web/` three.js client; region scopes 1x1/3x3/5x5 centered on region `5c87` = Hotan).
**Three testing/hardening passes are done.** Pass 1: query log panel + fixed layer-aware path
height (DP), long-segment densification, and a cross-region stairway-link bug. Pass 2:
fixed the **object-stitch vertical "teleport"** - object outline edges flagged `0x08`
(object<->object stitch) were wrongly usable as terrain->object on-ramps; only `0x00`
(object<->terrain) should be; plus a blocked-terrain-walk guard, a reworked height DP, and a
compass + `0x00`/`0x08` stitch-edge toggle. Pass 3: fixed object<->object (`0x08`) **seam
crossing** - where two coplanar object floors abut, their two near-coincident link edges form
a degenerate "corridor" that produced spurious "No path" and ~60-unit detours; coincident
seams are now crossed **directly** object-to-object (like a `0x00` on-ramp). Pass 3 also
guarded a Polyanya degeneracy crash in `third_party/Pathfinder` (an authorized exception to
the no-`third_party` rule, preserved as an in-repo patch) and added an agent-radius overlay.

Current task: **resume testing the GUI path queries** (stacked-surface / cross-region /
failure + the now-fixed `0x08` seam cases); **validate the coincident-seam threshold**
(`kCoincidentSeamEpsilon=8.0`) against real fortress bridges so a genuine bridge isn't
misclassified; and **run the Linux `bot` regression** for the four shared `silkroad_lib`
pathfinding changes (top open risk - they are macOS/viz-validated only), then scale up.

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
