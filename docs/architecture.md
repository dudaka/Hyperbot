# Architecture Overview

## What Hyperbot is

A platform for automating **Silkroad Online**. It does *not* read game memory or
automate the mouse/keyboard. Instead it is a **packet proxy**: it speaks the game's
network protocol directly, sitting between the game client and the servers. This lets it
both observe everything the server tells the client and act by sending packets as if the
client had sent them - including running with **no game client at all** ("clientless").

The long-term ambition (see root `Readme.md`) is a self-sufficient bot that can level a
character to cap and build the strongest possible character autonomously. The current
concrete focus is a **reinforcement-learning combat agent** trained by self-play PvP.

## Components (and where the code is)

| Component | Dir | Platform | Role |
|---|---|---|---|
| **Bot** | `bot/` | Linux | The brain. Proxy, packet pipeline, event broker, state machines, world model, RL training manager. Embeds CPython for JAX. |
| **silkroad_lib** | `silkroad_lib/` | all | Shared library: PK2 game-data parsing, navmesh, position math, scalar types, Blowfish. |
| **ui_proto** | `ui_proto/` | all | Protobuf schemas shared between bot and UI. |
| **rl_ui** | `rl_ui/` | Win/Linux | Qt6 dashboard for monitoring/controlling RL training. |
| **client_manager** | `client_manager/` | Win32 | Separate process that launches game clients and reaps dead ones. |
| **loader_dll** | `loader_dll/` | Win32 | Injected into the game client; hooks `connect()` to redirect traffic to the proxy. |
| **ui** | `ui/` | - | **Deprecated** legacy Qt UI. Do not touch. |

## Process / deployment topology

```
   Windows host(s)                         Linux host (the bot)
 +---------------------+               +-----------------------------+
 |  sro_client.exe     |   TCP (game   |   hyperbot                  |
 |   + loader_dll  ----+---protocol)---+-> Proxy -> PacketProcessor  |
 |  (connect() hooked) |               |     |         |             |
 +---------------------+               |     |         v             |
 |  client_manager <---+--ZeroMQ REP---+-----+      EventBroker       |
 |  (launches clients) |               |               |             |
 +---------------------+               |               v             |
                                       |        State machines       |
                                       |               |             |
   Any host                            |               v             |
 +---------------------+               |        TrainingManager       |
 |  rl_ui (Qt)     <---+--ZeroMQ ------+-> RlUserInterface            |
 |  dashboard          |  REQ/REP :5555|        |                    |
 |                     |  PUB/SUB :5556|        v (pybind11)         |
 +---------------------+               |   embedded CPython + JAX     |
                                       +-----------------------------+
```

The bot can also run **clientless** - the `Proxy` connects straight to the game servers
and synthesizes the keepalive packets a real client would send, so `client_manager`,
`loader_dll`, and `sro_client.exe` are not needed once login credentials are configured.
This is what the RL self-play uses to run many characters cheaply.

## The central data flow

Everything in the bot is organized around one loop:

```
  raw bytes off the socket
        |
        v
  SilkroadConnection / SilkroadSecurity   (decrypt, deframe)
        |
        v
  PacketProcessor.handlePacket(PacketContainer)
        |  parse into a typed ParsedPacket (dynamic dispatch by opcode)
        |  mutate WorldState (entity positions, HP, inventory, buffs...)
        v
  EventBroker.publishEvent(Event)          (e.g. EntityHpChanged, SkillEnded)
        |
        v
  every subscribed StateMachine.onUpdate(event)
        |  decide what to do; build a packet
        v
  Bot.injectPacket / Proxy.inject  -->  back into the pipeline / out to the server
```

Two properties make this clean:

- **Injected packets re-enter the *incoming* stream** (`InjectAsReceived`). The bot's own
  actions are parsed and evented exactly like real traffic, so state machines can observe
  the consequences of their own actions uniformly.
- **State machines never block.** They return `kDone`/`kNotDone` and rely on future
  events (or timer-driven delayed events) to advance. Time-based behavior (cooldowns,
  timeouts, "try again in 333 ms") is expressed as delayed events via the `TimerManager`.

## Threads

The bot is multi-threaded; `WorldState::mutex` is the main guard for shared game state.

- **Network/IO thread(s)** - Boost.Asio drives the proxy sockets, calls
  `PacketProcessor` (holding the world-state lock during state mutation).
- **EventBroker timer thread** - a min-heap of timers fires instant and delayed events,
  dispatching to subscribers.
- **RL training thread** - `TrainingManager::run()` blocks here: samples the replay
  buffer and calls into JAX to train, concurrently with action selection.
- **RlUserInterface threads** - a ZeroMQ request/reply loop (commands), a heartbeat
  broadcaster, and an event-queue-size sampler.
- **Main thread** - embeds the Python interpreter, warms up JAX, constructs `Hyperbot`,
  and runs.

`bot/src/main.cpp` is the entry point: it starts CPython via
`pybind11::scoped_interpreter`, adds the source dir to `sys.path`, warms up JAX, releases
the GIL, then constructs and runs `Hyperbot` (`hyperbot.cpp`), which parses config, loads
PK2 game data, and starts the training manager / sessions.

## How the subsystems relate

- **`Hyperbot`** owns global, character-independent state: parsed config and the loaded
  `GameData` (PK2 reference data).
- **`Session`** bundles everything for one controlled character: a `Proxy`, a `Bot`, a
  `PacketProcessor`, and a `ClientManagerInterface`.
- **`Bot`** is the per-character orchestrator. It subscribes to all events, owns the
  root state machines (login, then botting or pvp-manager), holds a pointer to the
  `Self` entity, and exposes helpers (pathfinding, skill checks) plus the RL interface.
- **`TrainingManager`** sits above sessions: it spins up multiple sessions, pairs them in
  PvP, collects observations/actions/rewards, owns the replay buffer, and drives the JAX
  training loop and checkpointing.

Continue with [networking.md](networking.md) for how packets actually get in and out.
