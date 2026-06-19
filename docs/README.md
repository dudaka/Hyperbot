# Hyperbot Architecture Docs

Structured technical documentation of how Hyperbot works, derived from reading the
codebase.

## Project status / current focus

- The codebase has been mapped end to end (see the docs below). These are the durable
  context files; keep them correct and consistent as work proceeds.
- **Active goal:** a **three.js visualization** that renders the navmesh (terrain + BMS
  objects across regions) and interactively queries paths between two points, using
  Hyperbot's **pathfinding/navigation as the backend**. It is **implemented and runnable** in
  `tools/navmesh_viz/` (standalone C++ service + `web/` three.js client; region scopes
  1x1/3x3/5x5/7x7 on `5c87` plus a 9x9 on `5a87`, and **load-by-zone**).
- Status, how-to-run, key findings, gaps, and next steps are in
  **[threejs-visualization-plan.md](threejs-visualization-plan.md)**; the backend authority
  is **[pathfinding.md](pathfinding.md)**. Current task: **run the Linux `bot` regression**
  for the shared pathfinding changes (top risk), validate the coincident-seam threshold, and
  continue GUI path-query testing - then the scaling/quality work listed there.

## Read in this order

1. **[architecture.md](architecture.md)** - The big picture: components, processes,
   threads, and the end-to-end data flow. Start here.
2. **[networking.md](networking.md)** - The MITM proxy, Silkroad's encryption handshake,
   DLL injection / connection redirection, and the packet parse/build framework.
3. **[state-machines.md](state-machines.md)** - The event broker, timer manager, and the
   hierarchical state-machine engine that drives all gameplay behavior.
4. **[world-model.md](world-model.md)** - The in-memory world (entities, inventory),
   the type-id classification system, the coordinate system, and PK2 game-data + navmesh.
5. **[pathfinding.md](pathfinding.md)** - Deep dive on navigation: the 2.5D model, the
   PK2 -> Navmesh -> Triangulation -> Polyanya pipeline, the height lifecycle, surface
   selection, layer transitions, coordinate frames, and known limitations. **Backend for
   the three.js demo.**
6. **[reinforcement-learning.md](reinforcement-learning.md)** - The RL combat agent:
   observation/action/reward design, the DDQN+PER training loop, self-play PvP, the
   C++/JAX boundary, and checkpointing.
7. **[ui-and-ipc.md](ui-and-ipc.md)** - The ZeroMQ + protobuf transport, the protobuf
   message catalog, the Qt rl_ui dashboard, config files, and the build system.
8. **[threejs-visualization-plan.md](threejs-visualization-plan.md)** - Status & handoff
   for the 3D navmesh/pathfinding visualization (`tools/navmesh_viz/`): what exists, how to
   run/test, resolved decisions, key findings, known gaps, and next steps.

> These docs describe the code as of the reading. File:line references can drift; treat
> the named files/classes as the source of truth and re-confirm specifics before relying
> on a line number.

## One-paragraph summary

Hyperbot sits as a transparent TCP proxy between a Silkroad Online game client (or no
client at all - "clientless") and the game servers. An injected DLL redirects the
client's connections to the proxy. The proxy decrypts Silkroad's Blowfish-secured
protocol, parses each packet into a typed object, updates a central `WorldState`, and
publishes an `Event`. A tree of composable state machines subscribes to events and
reacts by building and injecting packets - this is how the bot logs in, walks, fights,
loots, shops, and enhances gear. On top of this, a reinforcement-learning system trains
a combat policy by self-play PvP. Navigation is handled by a 2.5D navmesh + the Polyanya
pathfinder; that subsystem is the intended backend for an upcoming three.js visualization.
