# Playerbots v1: architecture

`mod:` = `modules/mod-playerbots/`. Core paths are relative to `src/server/`.

## Decision

**(C) A utility-weighted state machine with per-profile weights, built by refactoring the existing RpgStrategy, in the module directory. v1 adds no new core files.**

This is not a new system. `RandomChangeStatus` (`mod:src/Strategies/Rpg/RpgStrategy.cpp:89`) already rolls a weighted status from `Playerbots.Rpg.Weight.*` config keys, and AC's NewRpg does the same (`AC: NewRpgBaseAction.cpp:1211`). v1 changes four things:
1. Weights move from config into a world-DB profile table (`playerbots_behavior_profile`). They get per-profile overrides and `.bot profile reload`.
2. Each NPC service status becomes a short scripted sequence that replays the client's handler order (open → list → act → close; FEASIBILITY §c).
3. Movement goes through one mmaps-only `BotMover` (FEASIBILITY §d).
4. A world-thread scheduler and three tiers handle performance (PERFORMANCE.md).

The trigger/action Engine (A) (`mod:src/Engine/`) stays for chat commands, and later for combat. Rpg statuses do not move into it.

## Options compared

| | (A) AC-style strategy/trigger/action engine | (B) SQL rows (event, condition, action, weight) over a C++ action library | (C) weighted state machine + per-profile weights |
|---|---|---|---|
| Tick cost | every AI tick scans active strategies' triggers, sorts by relevance (`mod:src/Engine/Engine.cpp`; PerfMonitor times the scan) | per tick: evaluate the conditions of the rows whose event fired; cost grows with row count | a decision only when the running behavior ends (~10-60 s); otherwise one step of the running behavior |
| Tune without recompile | no (C++ triggers and relevance numbers) | yes, full | yes for weights, durations, cooldowns and enable flags; new behaviors need C++ |
| Complexity | high: 1.5k LOC engine plus a trigger/value per decision; hard to read why a bot did X | high: needs a condition evaluator, or a new ConditionMgr source type (**core patch**); action params are opaque ints like smart_scripts; debugging by DB diff | low: one enum of behaviors, each a small sequence; `.bot inspect` shows the state and the last transitions |
| Fit for v1 (non-combat, a few behaviors) | overkill | flexibility v1 does not need | matches what exists |
| Fit later (combat) | good, already present | poor for rotations | not for combat; (A) handles it |

**Recommendation: C** for town life, plus the existing A for commands and combat. B is rejected for v1: its main gain, conditions per row, duplicates ConditionMgr, and doing it properly needs a core patch.

## In-core vs isolated module

| | In core (`src/server/game/...`) | Module (`modules/mod-playerbots`) |
|---|---|---|
| Merge cost with master | every touched file conflicts | only the hook surface below conflicts |
| Build | full worldserver | `make modules` ~30 s (NOTES.md) |
| Access to core | full | public API plus the patched hooks |
| Status | — | already set up: modules CMake framework, module SQL updater |

**Decision: module.**

## Core patch surface (existing; v1 adds none)

**User directive 2026-10-15: no core edits at all costs.** No phase adds core changes. Removing the existing patches below is planned in CORE_EXIT.md: the modules framework, the `OnUpdate` hook and the BotPerf/perf-gate code leave core (P0b). The kept **minimal kit** is the bot session + login + frozen-bot skips in 5 files (user decision).

| Core file | Lines | Why it is needed | Owner |
|---|---|---|---|
| `game/Server/WorldSession.h` / `.cpp` | `.h:986` ctor flag, bot accessors; `.cpp:145,191,197,262-274,381,395,539,582,729,754,1494` | socketless session; packets to a callback; no online flag, idle kick or time sync; frozen flag | bots (committed) |
| `game/Handlers/CharacterHandler.cpp` | `:1313` `LoginBotPlayer` | client-less login through `LoginQueryHolder` | bots (committed) |
| `game/Entities/Player/Player.cpp` | `:897-902` frozen early return; `:1103` `OnPlayerUpdate` | per-bot AI tick on the map thread; cheap frozen bots | bots (committed) |
| `game/Maps/Map.cpp` | `:749,799` | frozen bots skip the map session update and `VisitNearbyCellsOf` | bots (committed) |
| `game/Scripting/ScriptMgr.h` / `.cpp` | `.h:793,1250`; `.cpp:2150` | `PlayerScript::OnUpdate`, which is **not upstream** | bots (committed) |
| `CMakeLists.txt`, `worldserver/CMakeLists.txt`, `worldserver/Main.cpp`, `database/Updater/DBUpdater.*`, `UpdateFetcher.*` | — | AC-style `modules/` loader and module SQL | framework (committed) |
| `game/Server/BotPerf.{h,cpp}` (**untracked**) + probe scopes in `World.cpp`, `MapManager.cpp`, `Map.cpp`, `GridNotifiers.cpp`, `Player.cpp`, `CharacterHandler.cpp`, `WorldSession.cpp` | — | read-only cost probes | scale benchmark workstream (uncommitted) |
| Gated perf changes: `Maps/Map.{h,cpp}` (`LazyMMap`, `SleepIdleActiveObjects`), `Movement/PathGenerator.cpp:79`, `Grids/Notifiers/GridNotifiers.cpp:237-246`, `MapManager.{h,cpp}` | — | scale gates, all OFF | scale benchmark workstream (uncommitted; FINDINGS_AND_PLAN.md) |

Merge risk is concentrated in `WorldSession.cpp` and `Player::Update`. The hook could be upstreamed as a small PR (`PlayerScript::OnUpdate`), which would remove one local patch.

## Component layout (module)

```
WorldScript::OnUpdate (world thread)
  RandomBotMgr        accounts, login batches, placement, homes        (existing)
  PlayerbotMgr        bot session updates (THREADUNSAFE opcodes)       (existing)
  BotScheduler        tiers, per-map tickets, caps, pause state        (NEW)
  WorldActionQueue    THREADUNSAFE service steps, home/anchor writes   (NEW)
  BotStatusBoard      snapshot for chat modules and GM inspect         (NEW)
PlayerScript::OnUpdate (bot's map thread)
  PlayerbotAI::Update  runs only with a ticket
    RpgStrategy        weighted behavior pick from profile             (REFACTOR)
    Behaviors          idle/wander/hop/emote/mount/taxi/services       (REFACTOR + NEW)
    BotMover           mmaps-only paths, fall watchdog                 (NEW, replaces MoveToward)
Startup (world thread, read-only after)
  HubIndex, PhaseGuard, TravelGraph, HotspotMgr, ProfileStore          (existing + NEW ProfileStore)
```

## Chat/intent hook (for later modules)

`BotStatusBoard`: one `BotStatus {tier, behavior, intentTarget (entry/guid/name), destinationArea, since, lastResult}` per bot.
- Written on every behavior transition, under a mutex, as a copy.
- Read API: `sPlayerbotMgr->GetStatus(guid)`.
- Observers registered with `OnBotTransition(callback)` are called on the world thread.

No core change, and nothing reaches into `PlayerbotAI` internals (H: design; there is no hook today, `mod:src/PlayerbotAI.h:70-108`).

## Scope update (2026-10-15)

The user added identity rules (IDENTITY.md: faction-safe spawning, racial mounts + riding, class armor/spec gear with AC-autogear-style commands) and a v2 track (GROUP_CONTENT.md: BG fill + objectives, random guilds, Assisted Combat rotations, role positioning, hazard avoidance).

The architecture holds:
- town life stays (C);
- combat uses the existing Engine (A) plus the rotation runner with the hand-written `.rot` scripts. Assisted Combat is an offline reference for tuning them (user decision);
- hazards are a generic module layer with a data hint table, with module shape geometry (user decision: no core patch). The pilot is Firelands;
- bot guilds use AC defaults (≤20 guilds, ≤15 members, no human recruiting).

No new core changes.

## Design rules carried from earlier decisions

- Behave like a typical player: stand rather than sit, emote occasionally, mount for distance, use services in towns (memory: playerbots act like players).
- Two tiers: real behavior only near humans; far bots are abstract and cheap. World objects never gate bot abilities (memory: two-tier scale).
- mmaps-only movement; no Z-snap teleports for visible bots (user, 2026-10-15).
- Default world data is incomplete: every behavior has a fallback, retries are bounded, and there are cooldowns per GUID.
