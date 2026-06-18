# UI, IPC, Config, and Build

How the bot talks to the monitoring UI and the client manager, what messages flow, and
how the whole thing builds. Code lives in `ui_proto/`, `bot/src/ui/`, `rl_ui/`,
`bot/src/config/`, and the CMake files.

## Transport: ZeroMQ + protobuf

The bot and the Qt `rl_ui` communicate over **ZeroMQ** (cppzmq) carrying **protobuf**
payloads. Two channels:

| Channel | Sockets | Port | Use |
|---|---|---|---|
| Commands | bot `REP` <- ui `REQ` | tcp `:5555` | request/reply: ping, "what's your broadcast port", start/stop training, checkpoint ops |
| Telemetry | bot `PUB` -> ui `SUB` | tcp `:5556` | fire-and-forget stream: heartbeat, plot points, character status, Q-values, checkpoints |

`client_manager` uses a separate ZeroMQ `REP` socket to receive client-launch requests
and heartbeats from the bot (`bot/src/clientManagerInterface.*`).

`bot/src/ui/rlUserInterface.*` is the bot side: a request loop (handles commands and
republishes them as events on the `EventBroker`), a heartbeat broadcaster (~250 ms), and
an event-queue-size sampler. Long operations (training, checkpoint save) are dispatched
as events and acked immediately, never blocking the socket.

> `bot/src/ui/userInterface.*` is an older, mostly-disabled world-state broadcaster
> (entities/items/movement) using `broadcast.proto` / `request.proto`. The RL UI uses
> `RlUserInterface` and `rl_ui_messages.proto`.

## Protobuf catalog (`ui_proto/`)

`rl_ui_messages.proto` is the main one:

- **RequestMessage** (ui->bot): `request_broadcast_port`, `ping`, or an `AsyncRequest`.
- **AsyncRequest**: `start_training`, `stop_training`, `request_checkpoint_list`,
  `save_checkpoint`, `load_checkpoint`, `delete_checkpoints`, `request_character_statuses`.
- **ReplyMessage** (bot->ui): `broadcast_port`, `ping_ack`, `async_request_ack`.
- **BroadcastMessage** (bot->ui, ~26 variants): `heartbeat`, `launch`, checkpoint
  state (`checkpoint_list`, `saving_checkpoint`, `checkpoint_loaded`,
  `checkpoint_already_exists`), and live telemetry (`plot_data {name,x,y}`,
  `character_status {hp,mp}`, `character_q_values`, `skill_cooldowns`, `item_count`,
  `active_state_machine`).

Supporting schemas: `rl_checkpointing.proto` (checkpoint registry: model/target/optimizer
paths, step count, timestamp), `rl_ui_config.proto` (ui connection settings),
`character_config.proto` + `server_config.proto` (bot config, below), `entity.proto`,
`position.proto`, `stats.proto`, `broadcast.proto`, `request.proto`,
`client_manager_request.proto`, `old_config.proto`.

## The Qt rl_ui app (`rl_ui/`)

A monitoring/control dashboard.

- `main.cpp` loads `Config` and shows `MainWindow`.
- `hyperbotConnectWorker` (a `QThread`) performs the connect handshake: ping ->
  request broadcast port -> emit `connected(port)`.
- `hyperbotSubscriberWorker` (a `QThread`) subscribes to `:5556`, emits a Qt signal per
  broadcast message, and treats >750 ms without a heartbeat as a disconnect.
- `hyperbot.*` translates protobuf broadcasts into Qt signals and outgoing commands into
  `AsyncRequest`s.
- `dashboardWidget` shows per-character HP/MP, current state machine, skill cooldowns,
  Q-values, item counts, and live plots (`interactiveChartView`, Qt Charts).
- `checkpointWidget` lists checkpoints and offers save/load/delete.
- `config.*` persists connection settings (ip/port/auto-connect) as a text protobuf.

## Configuration (`bot/src/config/`)

- **`ServerConfig`** - mainly the path to the Silkroad game client (for PK2 + launching).
- **`CharacterConfig`** - per character: credentials, `AutopotionConfig` (HP/MP/vigor
  thresholds, pill toggles), and `TrainingConfig` (training-area center + radius, and the
  attack / training-buff / non-training-buff skill id lists). `Training` reads these.

Configs are protobuf, stored as text for easy editing. The `--character` flag to the bot
selects which character config to use.

## Build system

- **C++20**, CMake (>=3.20), dependencies via **vcpkg** (`vcpkg.json`: abseil,
  boost-asio/bind/dll/filesystem, cppzmq, protobuf, gtest) plus git submodules in
  `third_party/` (`pybind11`, `tracy`, `gli`, `pathfinder` - see `.gitmodules`).
- **Targets gated by options** (root `CMakeLists.txt`): `BUILD_BOT` (Linux only - it
  finds Python3 and embeds it), `BUILD_UI` (needs Qt6), `BUILD_CLIENT_MANAGER` (Windows
  32-bit only, also builds `loader_dll`). `ui_proto`, `silkroad_lib`, `third_party`
  always build.
- **Presets** (`CMakePresets.json`): `linux_hyperbot`, `linux_rl_ui` (Ninja),
  `win64_qt_rl_ui` (NMake/JOM), `win32` (Visual Studio, for client manager).

```bash
# bot (Linux)
cmake --preset linux_hyperbot
cmake --build --preset linux_hyperbot

# rl_ui (Linux)
cmake --preset linux_rl_ui
cmake --build --preset linux_rl_ui
```

The Python RL environment is separate, under `bot/src/rl/python/` (managed with `uv`; see
`requirements.txt`). The embedded interpreter adds the bot source dir to `sys.path` and
imports `rl.python.dqn`, so the venv with JAX/Flax/Optax/Orbax must be active when running
the bot. Optional extras: `USE_ASAN` (AddressSanitizer; set `JAX_PLATFORMS=cpu`),
`TRACY_ENABLE` (Tracy profiler).
