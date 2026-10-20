# Playerbots v1: performance, pause and throttle

Target (user, 2026-10-15): **100 ACTIVE, 1000 logged in.** `mod:` = `modules/mod-playerbots/`. Core paths are relative to `src/server/game/`. Measured numbers come from `perf/FINDINGS_AND_PLAN.md` in the local tracking folder (single runs, ±15%, no human online).

## Cost model (measured; M because single runs)

| Item | Cost | Source |
|---|---|---|
| Idle world | 5.4 GiB | FINDINGS "CURRENT STATE" |
| First login into the big continents | ~3 GiB one time (navmesh/VMAP init) | same |
| Per loaded grid | ~34.5 MB (31.2 MB navmesh tile), ~400-600 ms to load | same |
| Per bot | ~0.47-0.52 MB | same |
| 750 bots, all gates on, nobody online | 9.4-10 GiB, ~33% of one core | same |
| CPU drivers | active GOs (elevators/doors) keep creatures updating (~340-500 ms/s at 200 bots); per-bot module loops every world tick | same |
| Grid creation | `Map::LoadGridsInRange` from `DelayedUnitRelocation`: every relocating player, bots included | same |

**Conclusion: grids cost memory, not bots.** 1000 online is affordable *if* frozen bots stay put and do not path (no tile loads). ACTIVE bots near humans load no extra grids, because the human already did.

Character saves: `PlayerSaveInterval = 90000` (`docker/etc/worldserver.conf:393`). Periodic save runs inside `Player::Update` (`Entities/Player/Player.cpp:1026-1035`), *after* the frozen early return (`:897`), so frozen bots never auto-save (H). 100 ACTIVE bots → ~1.1 saves/s.
- Plan: the module stretches bot saves to 10 min via `Player::SetSaveTimer` (`Entities/Player/Player.h:2665`) after each save (M).
- Bots are saved on logout and on pause.

## Tiers

| Tier | Meaning | Core state | AI | Movement | Today |
|---|---|---|---|---|---|
| **ACTIVE** | real behavior; a human may see it | not frozen | `PlayerbotAI::Update` at `UpdateInterval` (250 ms, `conf:28`) **only with a ticket** | BotMover (mmaps) | "simulated" (`mod:src/PlayerbotAI.cpp:153` `IsSimulated`) |
| **THROTTLED** | abstract life, low frequency | `SetBotFrozen(true)` (`Server/WorldSession.h` `IsBotFrozen`): skips `Unit::Update`, map session update and cell visits (`Maps/Map.cpp:749,799`, `Player.cpp:897`) | session tick every `FrozenTickMs` (5 s default behavior); virtual travel/state changes on timers (`mod:src/Travel/RouteFollower.cpp` `StepVirtual`) | none (teleport hops between verified nodes) | "frozen" |
| **PAUSED** | stopped by GM or cap | frozen | `PlayerbotAI::Update` returns at the top; no virtual travel, no timers | none | **new** |

Transitions run on the world thread in `BotScheduler`:
- PAUSED ← GM command, a global/zone pause flag, or over `MaxOnline`.
- ACTIVE ⇄ THROTTLED by the activation rule below, with `ForceGraceSeconds` (10 s, existing) hysteresis so bots don't flicker.

## Activation rule (who may be ACTIVE)

1. **Candidates:**
   - bots within `BotActiveAlone.Radius` (150 yd) of a human, or in a human's group or instance (existing force rules, `mod:src/PlayerbotAI.cpp:228` `IsActiveAlone`);
   - plus rotation slots only if `BotActiveAlone > 0`. Keep it at 0: rotation leaks memory, see FINDINGS R3.
2. **Caps (counts):** global `Playerbots.MaxActive` (100), then the existing `BotActiveAlone.ZoneMaxActive`, then `RadiusMaxActive` (`conf:633-634`).
3. **Cap beats proximity.** If >100 bots are near humans (e.g. a capital), the bots nearest to a human win; the rest go THROTTLED. A THROTTLED bot near a human stands still and looks idle, which is acceptable.
4. **SmartScale** (existing, world-diff driven) lowers `MaxActive` when the slowest tick in the last 10 s exceeds the ceiling.

## Per-tick budget and round-robin

Each Map updates on one thread, and maps run in parallel with `MapUpdate.Threads > 1`. So the **budget is per map**: `Playerbots.Budget.MapTickMs` (default 4 ms, set after the P0 baseline). The global limit is the count cap above.

Scheduler (world thread, end of `WorldScript::OnUpdate`, after maps; `World/World.cpp:2437`):
1. For each map, take ACTIVE bots whose AI interval is due.
2. Sort by priority:
   - (a) human distance bucket (<30 yd, <100 yd, <150 yd, group/instance);
   - (b) time since the last granted tick (older first), which is the round-robin.
3. Grant tickets in that order while `Σ EWMA(bot AI cost)` ≤ `MapTickMs`. Always grant at least 1 per map.
4. Starvation guard: a bot not granted for `Budget.MaxDeferMs` (2 s) gets granted first next tick. A bot deferred more than 10 times in a minute is demoted to THROTTLED (logged).
5. Map thread: `PlayerScript::OnUpdate` (`mod:src/Script/PlayerbotsScripts.cpp:98`) runs `PlayerbotAI::Update` only if `_ticket.exchange(false)`, measures its cost, and stores the EWMA (per-bot field, single writer).

Cost: one sort per map per world tick over ≤100 entries, plus one atomic per bot (H design; M cost until measured).

## Global and per-zone caps

| Key | Default | Meaning |
|---|---|---|
| `Playerbots.MaxActive` | 100 | ACTIVE count cap (new) |
| `Playerbots.MaxOnline` | 1000 | logged-in cap; excess bots log out (new; `Random.Count` stays the population size) |
| `Playerbots.BotActiveAlone.ZoneMaxActive` | 0 (off) | per-zone ACTIVE cap (existing) |
| `Playerbots.BotActiveAlone.RadiusMaxActive` | 0 (off) | per-human radius cap (existing) |
| `Playerbots.Budget.MapTickMs` | 4 | per-map AI budget per world tick (new) |
| `Playerbots.Budget.MaxDeferMs` | 2000 | starvation guard (new) |

## GM commands

Existing (`mod:src/Script/PlayerbotCommandScript.cpp:71-101`): `.bot add|remove|list|perf`, `.bot hotspot list|go`, `.bot travel info|nodes|route`, `.bot rotation reload|info`.

New:

| Command | Effect |
|---|---|
| `.bot pause <name\|zone\|all>` | → PAUSED; bot saved; stays logged in |
| `.bot resume <name\|zone\|all>` | PAUSED → THROTTLED (scheduler decides ACTIVE) |
| `.bot spawn <n> [hub]` | log in n random bots at a hub (or their home) |
| `.bot inspect <name>` | tier, behavior, target, destination, last 10 transitions with result, AI EWMA, ticket deferrals, path failures |
| `.bot stats` | tier counts, per-map budget use, tickets granted/deferred, world queue depth/drops, stuck/fall rescues in the last 10 min |
| `.bot profile reload` | reload `playerbots_behavior_profile` |

All on the world thread; RBAC as the existing commands.

## Metrics logging

Logger `server.playerbots.perf` through the existing `BotPerfReporter` (`mod:src/Perf/BotPerfReporter.cpp`), once per 60 s:
- tier counts;
- per-map AI ms p50/p95/max, tickets granted/deferred, demotions;
- per-behavior started/ok/fail/fallback;
- BotMover NOPATH/INCOMPLETE per map/tile, fall rescues, stuck;
- world queue depth/drops;
- saves/s.

New logger `server.playerbots.stuck`: one line per rescue/stuck/loop with guid, map, x, y, z, tile, behavior, target.

PerfMon must stay cheap when off. The core BotPerf probes are used only for the P0 baselines and are **removed in P0b** (no-core rule). After that every metric is module-side (`PerfMonitor`, scheduler EWMA, `BotPerfReporter`), each guarded by one relaxed atomic flag.
