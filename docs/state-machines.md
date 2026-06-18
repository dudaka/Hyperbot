# Events and State Machines

This is the bot's "decision" layer: an event broker that dispatches game events, and a
tree of composable state machines that react by injecting packets. Code lives in
`bot/src/broker/`, `bot/src/event/`, `bot/src/state/`, and `bot/src/bot.*`.

## The event system

### EventCode and Event

`event/eventCode.hpp` defines ~115 event types via an `EVENT_EVENTCODE_LIST(F)` X-macro
(generating an `enum class EventCode`). Categories: lifecycle (`SelfSpawned`,
`EntitySpawned`, `EntityDespawned`), vitals (`EntityHpChanged`, `MaxHpMpChanged`),
combat/skills (`SkillBegan`, `SkillEnded`, `DealtDamage`, `SkillFailed`), buffs
(`BuffAdded`/`BuffRemoved`), movement (`EntityMovementBegan`/`Ended`), inventory
(`ItemMoved`, `ItemUseSuccess`), cooldowns, PvP (`BeginPvp`, `ReadyForPvp`), and UI/RL
commands (`RequestStartTraining`, `RlUiSaveCheckpoint`, ...).

`event/event.hpp` has one struct per event carrying its payload (e.g. `EntityHpChanged`
holds the entity's global id). All derive from `Event` (which has a unique id + code).

### EventBroker (`broker/eventBroker.*`)

A thread-safe publish/subscribe hub.

- `publishEvent<T>(args...)` - construct and dispatch an event immediately.
- `publishDelayedEvent<T>(delay, args...)` - schedule an event for the future; returns an
  id so it can be cancelled (`cancelDelayedEvent`) or queried
  (`timeRemainingOnDelayedEvent`). This is how cooldowns/timeouts are modeled.
- `subscribeToEvent(code, handler)` / `unsubscribeFromEvent(id)`.

Subscriptions track active-caller counts so unsubscribe safely waits for in-flight
callbacks (prevents use-after-free).

### TimerManager (`broker/timerManager.*`)

Backs delayed/instant events. A min-heap of timers keyed on fire time, serviced by a
dedicated thread. When a timer fires it calls back into the EventBroker, which notifies
subscribers. "Instant" events also route through here so ordering is consistent.

## The state-machine engine

### Base class (`state/machine/stateMachine.hpp`)

```cpp
class StateMachine {
  virtual Status onUpdate(const event::Event *event) = 0;   // Status = {kDone, kNotDone}
  std::unique_ptr<StateMachine> childState_;  // hierarchical nesting
  StateMachine *parent_;
  Bot &bot_;                                  // access to world, broker, injection
};
```

Key ideas:

- **Non-blocking.** A machine inspects the event, maybe injects a packet, and returns
  `kNotDone` (still working) or `kDone` (finished). Progress is driven by subsequent
  events, including self-scheduled delayed events (timeouts, "retry in 333 ms").
- **Hierarchy.** A machine can hand off to a `childState_`; events flow down to the
  deepest active child first. The "active state machine name" reported to the UI is the
  deepest child's type name.
- **Opcode blocking.** `pushBlockedOpcode()` suppresses server/client packets during a
  delicate sequence (e.g. equipment swap); they unblock on destruction.
- **Packet injection** delegates up to the `Bot`/`Proxy`.

### Composition

- **`SequentialStateMachines`** - a deque run one at a time; when the front returns
  `kDone` it's popped and the next runs. Used for "do A, then B, then C".
- **`ConcurrentStateMachines`** - a vector all updated on each event; each is removed when
  it finishes. Used for "heal while attacking".

These compose arbitrarily, so complex behavior is built from small machines.

## The behavior catalog

State machines live in `state/machine/`. By category:

- **Lifecycle:** `login` (auth -> shard/character select -> spawn), `botting` (root grind
  loop: cycles `townlooping` <-> `training`).
- **Movement:** `walking` (waypoint following along a computed path; blocks manual move
  requests; emits `WalkingPathUpdated` for the UI).
- **Combat:** `castSkill` (cast a skill, auto-swapping weapon/shield/imbue into equip
  slots first, with cast timeouts), `intelligenceActor` (lets an RL "intelligence" choose
  and execute an action - see below).
- **Survival:** `autoPotion` (HP/MP/vigor potions, purification/universal pills),
  `ensureFullVitalsAndNoStatuses`, `dispelActiveBuffs`, `resurrectInPlace`,
  `waitForAllCooldownsToEnd`.
- **Items/inventory:** `pickItem`, `pickItemWithCos`, `moveItem`, `dropItem`, `useItem`,
  `equipItem`, `maybeRemoveAvatarEquipment`.
- **Town economy:** `townlooping`, `talkingToShopNpc`, `talkingToStorageNpc`,
  `buyingItems`, `sellingItems`.
- **Progression:** `applyStatPoints`, `maxMasteryAndSkills`, `alchemy` (gear enhancement),
  `spawnAndUseRepairHammerIfNecessary`.
- **PvP:** `pvpManager` (listens for PvP assignments, coordinates readiness),
  `enablePvpMode`, `freePvpUpdate`.
- **GM/testing:** `executeGmCommand`, `gmWarpToPosition`, `gmCommandSpawnAndPickItems`,
  `disableGmInvisible`.

`Training` (`state/machine/training.cpp`) is config-driven: it reads attack/buff skill
ids and the training-area geometry from the character config, then buffs, picks items,
selects monster targets in range, and attacks - delegating to `walking`, `castSkill`,
`pickItem`, etc. In RL mode it hands action selection to the `intelligenceActor`.

## The Bot orchestrator (`bot.cpp`)

`Bot::initialize()` subscribes a single handler to **every** event code (via the same
X-macro). `Bot::handleEvent()` does special-case bookkeeping (login transitions,
forwarding HP changes to the UI, start/stop training commands, etc.) and then forwards
the event to the active root state machine via `onUpdate()`. The `Bot` also exposes
gameplay helpers (`calculatePathToDestination`, `canCastSkill`, `getClosestNpcGlobalId`)
and the RL interface (`asyncLogIn`, `asyncStandbyForPvp`, `sendQValues`, ...).

## Adding a state machine

`tools/addNewStateMachine.py <Name>` scaffolds the `.hpp`/`.cpp` (a `StateMachine`
subclass with a stub `onUpdate`) and registers them in `bot/CMakeLists.txt`. Then wire it
into a parent (set it as a child, or emplace it into a sequential/concurrent machine).

Next: [world-model.md](world-model.md) - the state these machines read and reason over.
