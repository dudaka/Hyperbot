# World Model: Entities, Items, Coordinates, and Game Data

How the bot represents the live game world and the static reference data it loads from
the client. Code lives in `bot/src/state/`, `bot/src/entity/`, `bot/src/storage/`,
`bot/src/type_id/`, `bot/src/math/`, and `silkroad_lib/src/silkroad_lib/`.

## Live world state

### WorldState + EntityTracker (`state/`)

`WorldState` is the central, mutex-guarded registry of everything currently in view. It
holds an `EntityTracker` plus active-buff bookkeeping.

`EntityTracker` maps `EntityGlobalId -> shared_ptr<Entity>` and **reference-counts**
entities (multiple bot characters can see the same monster spawn, so spawn/despawn are
counted, not absolute). `entitySpawned()` returns true the first time an entity appears;
`entityDespawned()` returns true when the last reference goes away.

### Entity hierarchy (`entity/`)

```
Entity                      refObjId, 4-part typeId, globalId, position, angle
  +- MobileEntity           movement state, speeds, interpolated position()
       +- Character         lifeState, current/known HP, active buffs
            +- PlayerCharacter   name, free-PvP mode
            |    +- Self          the bot itself (see below)
            +- NonplayerCharacter
                 +- Monster       rarity, current target
  Item                      ground item: rarity, optional owner
  Portal
```

`MobileEntity` is notable: it stores when movement began and a destination, and its
`position()` **interpolates** the current position from elapsed time + speed. Arrival is
detected by a scheduled event. It can also register geometry boundaries (`entity/geometry`
- `Circle`/`Rectangle`) to know when it enters/exits, e.g. a training area.

`Self` (`entity/self.hpp`) is the rich one: level, experience, skill points, race/gender,
HP/MP and maxes, STR/INT, abnormal-status bitmask, masteries and skills, the
`SkillEngine` (cooldown tracking), all the storages (inventory, avatar, storage, guild,
COS, buyback), and UI/training flags (`trainingIsActive`, `trainingAreaGeometry`).

### Inventory / storage (`storage/`)

`Storage` is a sparse slot container (`ItemList` of `shared_ptr<Item>`, empty slots are
null). It supports move/add/withdraw/delete and rich search: by category, by type id, by
ref id. `Self` has several storages plus a fixed 5-slot `BuybackQueue`.

`Item` (in `storage/`, distinct from the on-ground `entity/item`) is a polymorphic
inventory item: `ItemEquipment` (opt level, durability, magic/socket/elixir options),
`ItemCosGrowthSummoner` / `ItemCosAbilitySummoner` (pets), `ItemExpendable` (stackable
consumables), `ItemStone`, `ItemMonsterCapsule`, `ItemMagicPop`. `newItemByTypeData()`
constructs the right subclass from the item's reference data.

## The type-id classification system (`type_id/`)

Silkroad classifies every object with a hierarchical **4-part type id** (typeId1..4, plus
cash-item and bionic flags), packed into a 16-bit value. `typeCategory.cpp` packs them:
`(id1<<2)|(id2<<5)|(id3<<7)|(id4<<11)`, with the low 2 bits the flags.

`TypeCategory` is a (data, mask) pair. A broad category (e.g. "any HP potion") uses a
shorter mask and matches many concrete items; a fully-specific category matches one. This
lets code ask "is this item a recovery potion?" without enumerating ref ids.
`type_id/categories.hpp` defines the hierarchy: Character -> {Player, NPC -> {Monster,
COS}}; Item -> {Equipment -> {Armor, Shield, Weapon -> {Sword, ...}}, Expendable ->
{RecoveryPotion -> {Hp, Mp, Vigor}, Scroll, Ammo, Currency}}; Structure.

**When extending the bot, match against these categories rather than hardcoding ref ids.**

## Coordinates (`silkroad_lib/.../position*`, `bot/src/math/`)

Silkroad positions are **region-relative**:

```cpp
Position { RegionId regionId; float xOffset, yOffset, zOffset; }
```

- World regions: `regionId = (zSector << 8) | xSector`; each region is **1920 units**
  square (`game_constants.hpp kRegionSize`). Dungeons set the high bit and use the low
  byte as a dungeon id.
- `Position` auto-normalizes: offsets outside [0,1920) roll over into the adjacent region.
- `position_math.*` provides distance/angle/offset/interpolation and sector<->regionId
  conversion.
- `math/pointTranslator.*` converts between world `Position` and a local pathfinder frame
  (centered on a reference point), so the pathfinder works in flat local coordinates and
  results convert back to world positions.

## Static game data: PK2 (`silkroad_lib/.../pk2/`)

The Silkroad client ships its data in **PK2** archives - a Blowfish-encrypted packed
file format (header + 20-entry blocks forming a directory tree). At startup
`GameData::parseSilkroadFiles()` opens the client's PK2s (via `Pk2Reader` /
`Pk2ReaderModern`, default key `"169841"`) and parses the reference tables the bot needs:

| Loader | Contents |
|---|---|
| `DivisionInfo` | server divisions + gateway IPs, gateway port |
| `CharacterData` | NPC/monster templates: stats, maxHP/MP, type id, default skills, exp given |
| `ItemData` | item templates: type id, price, requirements, attack/defense/durability ranges (~170 fields) |
| `SkillData` | skill definitions: cast/cooldown timings, range, target rules, costs, params; helpers like `isImbue()`, `durationMs()` |
| `MasteryData` | mastery trees |
| `MagicOptionData` | socket / elixir options |
| `LevelData` | exp curves and per-kill exp/cost by level |
| `RefRegion` | region metadata (names, links, battlefield flags) |
| `TextData` / `TextZoneNameData` | localized strings and zone names |
| `TeleportData` | teleport NPCs / gates |
| skill & item **icons** (gli textures) | for the UI |
| **navmesh** | walkability geometry (see below) |

`GameData` is loaded once and shared read-only by every `Bot`. It also offers
name/lookup helpers (`getSkillName`, `getItemName`, ...).

## Navmesh & pathfinding (summary)

The navmesh is the walkable-geometry map: `NavmeshParser` reads per-region terrain
(walkable tile grid + height field), region-border edges, and object meshes (BMS
buildings/bridges) from PK2 into a `Navmesh`; `NavmeshTriangulation` turns that into a
searchable mesh; and the external `Pathfinder` library (Polyanya) finds routes.
`Bot::calculatePathToDestination()` is the entry point that produces the waypoint list the
`walking` state machine follows.

The world is **2.5D** (stacked single-valued surfaces, not a 3D volume), and height is used
only to select which surface an endpoint sits on. This is the planned three.js demo's
backend, so it has its own deep-dive:

**-> Full details: [pathfinding.md](pathfinding.md)** (pipeline, height lifecycle, surface
selection, layer transitions, coordinate frames, known limitations). The forward-looking
demo brief is in [threejs-visualization-plan.md](threejs-visualization-plan.md).

Next: [reinforcement-learning.md](reinforcement-learning.md) - how all of this feeds a
learning agent.
