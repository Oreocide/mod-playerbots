# Playerbots: core-exit plan (user directive 2026-10-15: "avoid touching core files at all costs")

**Rule from now on:** no new core edits for any phase. Every v1/v2 design in these docs already meets that: no new core files. This doc covers the **existing** local core patches, what upstream offers instead, and what cannot leave core.

Upstream reference = `34fd0ebd` (TC master merged 2026-10-14). Core paths are relative to `src/server/`. Confidence H/M/L.

## Upstream facts that make an exit possible (H)

| Fact | Cite |
|---|---|
| `WorldMapScript::OnUpdate(Map*, diff)` runs inside `Map::Update` on that map's thread. `WorldMapScript` is **not** DB-bound: every registered script whose map id matches is called, so a module can register one per world map | `game/Scripting/ScriptMgr.h:346-358`; dispatch `game/Scripting/ScriptMgr.cpp:1582-1611`; call `game/Maps/Map.cpp:944` |
| `InstanceMapScript` / `BattlegroundMapScript` **are** DB-bound (script id from instance/BG template), so a module cannot add a second one to a dungeon, raid or BG | same, `:1584-1588` |
| `WorldScript::OnUpdate` runs on the world thread after all map threads finished, so it is safe to touch any map from there | `game/World/World.cpp:2437` |
| Module SQL: the updater already includes `$/sql/custom/world` and `$/sql/custom/characters` (`updates_include`, state RELEASED) | world/characters DB query 2026-10-15 |
| Module code: `src/server/scripts/Custom/` is compiled into the scripts library with its own `custom_script_loader.cpp` (exists: `token_shop.cpp`) | tree listing |
| `WorldSession::ResetTimeOutTime(bool)` is public | upstream `game/Server/WorldSession.h:1211` |
| `Player::LoadFromDB(ObjectGuid, CharacterDatabaseQueryHolder const&)` and `enum PlayerLoginQueryIndex` are public | upstream `game/Entities/Player/Player.h:1853,942` |

## Upstream facts that block a pure module (H)

| Blocker | Cite |
|---|---|
| `LoginQueryHolder` is a file-local class in CharacterHandler.cpp, so `HandlePlayerLogin(LoginQueryHolder const&)` cannot be called from a module | upstream `game/Handlers/CharacterHandler.cpp:70`; `WorldSession.h:1254` |
| `_recvQueue` only drains while `m_Socket[REALM]` exists, so a module cannot queue `CMSG_PLAYER_LOGIN` (or any packet) to a socketless session | upstream `game/Server/WorldSession.cpp:371` |
| `SendPacket` with no socket logs an **ERROR per packet** ("Prevented sending … to non existent socket") and drops it | upstream `WorldSession.cpp:254-257` |
| Idle check calls `m_Socket[REALM]->CloseSocket()`, which crashes on a null socket once `m_timeOutTime` passes | upstream `WorldSession.cpp:357-358` |

## Existing local core patches: exit options

| Patch (today) | Purpose | Module-only replacement | Cost of replacing | Recommendation |
|---|---|---|---|---|
| Modules framework: `CMakeLists.txt`, `worldserver/CMakeLists.txt`, `worldserver/Main.cpp`, `database/Updater/DBUpdater.*`, `UpdateFetcher.*` | build/load `modules/*`, apply module SQL | code → `src/server/scripts/Custom/playerbots/` (registered in `custom_script_loader.cpp`); SQL → `sql/custom/{world,characters}/` | move files; Custom is a TC-designated local area (one loader line), not core logic | **replace** (H) |
| `PlayerScript::OnUpdate` hook: `ScriptMgr.h:793,1250`, `ScriptMgr.cpp:2150`, `Player.cpp:902,1103` | per-bot AI tick on the map thread | `WorldMapScript` per world map (iterate that map's bots on its thread) + `WorldScript::OnUpdate` for bots in instances/BGs (world thread, after maps) | instance/BG bots lose parallelism (few of them; fine). Scheduler tickets stay | **replace** (H design, M until measured) |
| Frozen bots: `Player.cpp:897-902` early return, `Map.cpp:749,799` skips | far bots skip `Unit::Update`, session update and cell visits (big CPU/grid saver in the perf model) | (a) **park** far bots: log them out, log in on demand near humans (pure module); or (b) keep them online and pay the full `Player::Update` + grid cost | (a) changes "1000 online" to "1000 in the pool, ~N in world", with login churn; chat modules need online bots (memory: AC instance). (b) costs CPU/memory, to be measured in P0 | **decision needed** |
| Bot session: ctor flag, `SendPacket` redirect, skip idle/online/time-sync, `KickPlayer`/logout branches (`WorldSession.h/.cpp`), `LoginBotPlayer` (`CharacterHandler.cpp:1313`) | log a character in with no client; hand packets to the module | partial: idle crash avoided with `ResetTimeOutTime`; packets dropped (silence via `Logger.network.opcode` config) and module **polls** state instead (group/guild invites, BG/LFG proposals, duel, trade); login reimplemented in the module with its own `CharacterDatabaseQueryHolder` + `Player::LoadFromDB` + the add-to-world steps of `HandlePlayerLogin` | the login copy is ~300+ lines of core logic in the module that **silently drifts** on every master merge (new login steps missed → subtle bugs); polling replaces push for every invite type | **decision needed**: keep a minimal "bot session kit" patch (~3 files) vs duplicate login in the module |
| `BotPerf.{h,cpp}` + probe scopes in World/MapManager/Map/GridNotifiers/Player/CharacterHandler/WorldSession (uncommitted) | perf measurement | module-side `PerfMonitor` + `BotPerfReporter` only (lose per-phase core timings) | measurement detail | **decision needed** (scale-benchmark workstream) |
| Perf gates `LazyMMap`, `SleepIdleActiveObjects`, `SkipBotVisibilityGridLoad` (Map.h/.cpp, PathGenerator.cpp, GridNotifiers.cpp, MapManager.*; uncommitted, all OFF) | grid/navmesh memory savings | none in pure module | lose the gated savings | **decision needed** (same workstream) |

## Decisions (user, 2026-10-15)

| Item | Decision |
|---|---|
| Bot login / socketless session | **keep a minimal core kit**: `WorldSession.h/.cpp` bot branches + `LoginBotPlayer` (`Handlers/CharacterHandler.cpp`) |
| Frozen far bots | **keep** the frozen patch (`Player.cpp` early return, `Map.cpp` two skips) as part of the kit |
| BotPerf probes + 7 perf gates | **remove after Phase 0** (used for the baselines, then backed out together with every module reference to `BotPerf.h`, e.g. `modules/mod-playerbots/src/Travel/MoveToward.cpp`) |
| Modules framework | **leave core**: code → `src/server/scripts/Custom/playerbots/`, SQL → `sql/custom/` |
| `PlayerScript::OnUpdate` hook | **leave core**: `WorldMapScript` per world map + world-thread tick for instance/BG bots. The frozen block's `OnPlayerUpdate` call (`Player.cpp:902`) goes too; frozen bots get their abstract tick from the same map script |

**Final core footprint for bots ("minimal kit"):** `game/Server/WorldSession.h`, `game/Server/WorldSession.cpp`, `game/Handlers/CharacterHandler.cpp` (`LoginBotPlayer` only), `game/Entities/Player/Player.cpp` (frozen early return only), `game/Maps/Map.cpp` (two frozen skips). No other core file differs from upstream for bot reasons. The kit is frozen: no new lines without explicit user approval.

## Effect on earlier P0 items

- G1 (BotPerf untracked): becomes part of the BotPerf decision above. No core edit in P0.
- G2 (`ValidateAppearance` empty-choice guard): fixed **module-side**: `FillCustomizations` must never send an empty choice list for a race/gender that has options (`modules/mod-playerbots/src/RandomBotMgr.cpp:155`). The core guard stays whatever the working tree has; humans are a separate matter.
