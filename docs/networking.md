# Networking: Proxy, Security, and Packets

This is how Hyperbot gets between the game client and the server, decrypts the protocol,
and turns bytes into typed packets (and back). Code lives in `bot/src/proxy.*`,
`bot/src/silkroadConnection.*`, `bot/src/shared/`, `bot/src/packet*`, `loader_dll/`, and
`client_manager/`.

## 1. Getting in the middle (MITM)

Silkroad's client connects to a **gateway** server to log in, then to an **agent** server
for actual gameplay. Hyperbot intercepts both.

### DLL injection + connect() hook (Windows)

`client_manager` launches `sro_client.exe` **suspended**, injects `loader_dll` (via the
`edx_labs` helper in silkroad_lib), writes the proxy's listening port to a `<PID>.txt`
file the DLL reads, then resumes the process.

Inside the client, `loader_dll/loaderDll.cpp` detours the WinSock `connect()` function.
When the client tries to connect to a configured gateway/agent IP, the detour rewrites
the destination to `127.0.0.1:<proxy port>`. The client now unknowingly talks to the
proxy. `client_manager` also reaps dead client processes and is itself kept alive by
heartbeats from the bot (over ZeroMQ REP); if heartbeats stop, it kills its clients.

### The Proxy (`proxy.cpp`)

`Proxy` is a transparent TCP relay built on Boost.Asio. It holds two
`SilkroadConnection`s: `clientConnection` (client<->proxy) and `serverConnection`
(proxy<->server). On accept it generates the security handshake and connects outward to
either the gateway or, once login has progressed, the agent server.

- **Gateway -> agent transition:** when the gateway's `ServerGatewayLoginResponse`
  arrives carrying the agent server's IP/port/token, the proxy records them, sets
  `connectToAgent_`, and the next client connection is routed to the agent server.
- **Blocking opcodes:** the proxy can drop selected opcodes so the real client's actions
  (e.g. a manual move request) don't fight the bot. State machines push/pop blocked
  opcodes around critical sequences.
- **Clientless mode:** with no client, the proxy connects straight to the server and a
  keepalive timer injects `FrameworkAliveNotify` every few seconds.

## 2. Silkroad security layer

`bot/src/shared/silkroad_security.cpp` (a trimmed fork of "phconnector"/Weeman) implements
the wire protocol, using `sro::blowfish::Blowfish` from silkroad_lib.

- **Blowfish encryption** with a 64-bit key negotiated at handshake.
- **Security/count bytes** and a **CRC check byte** for packet integrity, derived from a
  64 KB security table built at startup.
- **Diffie-Hellman handshake** (`GenerateHandshake`) to agree the encryption keys.

The security object exposes a simple queue API: feed it raw bytes with `Recv()`; pull
fully-decrypted packets with `HasPacketToRecv()` / `GetPacketToRecv()`; enqueue outgoing
packets with `Send()`; pull wire-ready encrypted bytes with `GetPacketToSend()`.

### PacketContainer

The unit of currency (`shared/silkroad_security.h`):

```cpp
struct PacketContainer {
  enum class Direction { kClientToServer, kBotToServer, kServerToClient, kBotToClient };
  uint16_t opcode;          // packet type, e.g. 0x7074
  StreamUtility data;       // payload bytes + read/write cursor
  uint8_t encrypted;        // Blowfish?
  uint8_t massive;          // multi-part packet?
  Clock::time_point timestamp;
};
```

`StreamUtility` (`shared/stream_utility.*`) is the binary reader/writer used everywhere:
templated `Read<T>()` / `Write<T>()` over an internal byte vector, with read-error flags.

## 3. Parsing: bytes -> typed packet -> world update -> event

`PacketProcessor::handlePacket(PacketContainer)` (`bot/src/packetProcessor.cpp`) is the
hub:

1. `PacketParser::parsePacket()` (`packet/parsing/`) deserializes the payload into a
   concrete `ParsedPacket` subclass for that opcode.
2. A macro (`TRY_CAST_AND_HANDLE_PACKET`) `dynamic_cast`s to find the matching type and
   calls its handler (e.g. `serverAgentCharacterDataReceived`,
   `serverAgentEntitySpawnReceived`, `serverAgentEntityUpdateHpReceived`).
3. The handler mutates `WorldState` (under its mutex) and publishes the corresponding
   `Event` on the `EventBroker`.

There are ~80 parsers under `packet/parsing/`, one per server (and some client) packet of
interest - spawns, despawns, HP/MP/EXP updates, movement, skills, buffs, inventory,
login/auth, shards, etc.

## 4. Building: typed intent -> bytes -> injection

`packet/building/` mirrors parsing: each builder is a class of static factory methods
producing a `PacketContainer`. Example:

```cpp
PacketContainer p = ClientAgentActionCommandRequest::attack(targetGlobalId);
bot.injectPacket(p, PacketContainer::Direction::kBotToServer);
```

`Proxy::inject()` puts the packet into the **incoming** queue of the relevant
`SilkroadConnection` (`InjectAsReceived`), not the outgoing one. Consequence: the bot's
own packets pass through the same parse + event pipeline as real packets, so the rest of
the bot reacts to bot actions and real actions identically. (Outgoing-to-the-wire happens
later when the security layer's send queue is flushed to the socket.)

## 5. Opcodes

`bot/src/packet/opcode.hpp` defines all opcodes through a single
`PACKET_OPCODE_LIST(F)` X-macro (generating both the enum and name strings). A few
landmarks:

| Opcode | Meaning |
|---|---|
| `0x2002` FrameworkAliveNotify | keepalive (used in clientless mode) |
| `0xA102` ServerGatewayLoginResponse | carries agent server address - triggers gateway->agent switch |
| `0x3013` ServerAgentCharacterData | full character data on spawn |
| `0x3015` / group spawn | entity spawn |
| `0x7074` ClientAgentActionCommandRequest | attack / cast / pick (the main "do something" packet) |
| `0x7034` inventory operation | move/use items |

## 6. Codegen helpers

Adding protocol coverage is scripted:

- `tools/addNewPacketParser.py <Name>` - scaffolds a parser under `packet/parsing/` and
  registers it in `bot/CMakeLists.txt`.
- `tools/addNewPacketBuilder.py <Name>` - scaffolds a builder under `packet/building/`.

After scaffolding, add the opcode to `opcode.hpp` and (for parsers) a handler +
`TRY_CAST_AND_HANDLE_PACKET` line in `packetProcessor.cpp`, publishing any new event.

Next: [state-machines.md](state-machines.md) - what consumes these events.
