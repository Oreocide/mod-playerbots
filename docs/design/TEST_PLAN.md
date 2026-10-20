# Playerbots v1: test plan

Server: docker `trinity-worldserver`. Logs: `docker/logs/Server.log`. Poll with `tail -n N` only, never `docker logs --since` (it rescans a multi-GB json log).

## Rules for every run

1. Back up `docker/etc/worldserver.conf.d/playerbots.conf` (and `worldserver.conf` if touched). Run with an EXIT trap that restores it. After the run, verify with `md5sum` against the original and check `free -m`. (The host restarts the worldserver on whatever conf is on disk.)
2. Cap container memory for big cells (`docker update --memory`).
3. Stop the container before `make install` / copying the binary. Otherwise the old binary keeps running (`Text file busy`).
4. Record the git state (HEAD + `git diff --stat`) with each result.

## 1. Unit tests (no server)

`modules/mod-playerbots/tests/run_engine_test.sh` (g++ only, ~5 s), extended with pure tests for:
- profile weight pick: distribution over 10k rolls within ±3% of the weights; weight 0 never picked;
- scheduler ticket grant: budget respected; starvation guard grants a deferred bot within `MaxDeferMs`; priority order;
- loop guard: 3 failures in 10 min disable, re-enable after 30 min;
- snap check math: 2D/Z thresholds.

## 2. Functional checks (one bot, per phase)

Each ROADMAP phase has its own acceptance test. Evidence = `.bot inspect` output + `server.playerbots.stuck`/`perf` log lines + DB rows (e.g. `character_homebind`).

## 3. Headless soak (no human)

Prerequisites:
- 750 bot characters exist (`characters.playerbots_random_bots` event `placed` = 750, 2026-10-15). the user set the creation target to **3000** on 2026-10-16 (the scale-benchmark session creates them in the background, with its own DB backup in `trinitycore-tracking/backups/playerbots_precreate_2026-10-15/`); 1000 online is the soak target.
- This is the user's live server, so each run needs the user's go-ahead.

**Soak design note (advisor, 2026-10-16):** with nobody online and `BotActiveAlone = 0` every bot is frozen, and the map-thread paths the P0 locks protect (`UpdateHome` via `DetectInterruption`, the rpg anchor checks) never run. The P0/P1 soak therefore sets `Playerbots.Debug.ForceActive = 100` (about 100 random bots run full AI). Those bots load grids and navmesh tiles far from any player, so RSS creep and slower ticks are expected in this cell and the strict thresholds do not apply; keep `RSS_ABORT_MB=17000` and the container cap.

Config cell (test copy of `playerbots.conf`):
- `Random.Count = 1000`, `OnlyWithPlayers = 0`, `BotActiveAlone = 0`;
- `Debug.ForceActive = 100`, **confined to warm hubs**: forced bots are placed only in pre-warmed capitals (`Warm.Maps` gate) and do not path into cold grids. Forced-active bots far from humans are otherwise the rotation case FINDINGS R3 shows leaking memory, and each cold navmesh tile costs ~400 ms under `LazyMMap`;
- `MapUpdate.Threads = 4` (exposes races), PerfMon on for the measure run and off for the overhead run.

Durations: 1 h smoke, then 4 h, then 8 h.

The pass thresholds below are **relaxed for headless runs** (RSS trend ≤ +5%/h, world diff p95 ≤ +25 ms). The strict thresholds (RSS flat ±2%, p95 ≤ +10 ms) apply to the human-present check (§6).

Collected every 60 s:

| Metric | Source | Pass |
|---|---|---|
| world diff p50/p95/max | `server.playerbots.perf`, `.server info` | p95 within +10 ms of the P0 baseline |
| per-map AI ms p95 | scheduler metrics | ≤ `Budget.MapTickMs` |
| tier counts | `.bot stats` | ACTIVE ≤ 100; total ≤ 1000 |
| RSS / jemalloc allocated | container stats, BotPerf heap | flat ±2% after the first 30 min |
| saves/s | perf log | ≤ planned (100 active / 600 s) |
| behavior ok/fail | perf log | fail <10% per behavior, every fail has a reason |
| world queue depth/drops | perf log | depth < 200, drops only for logged-out/teleported bots |

## 4. Tick-cost measurement

- Per bot: EWMA of `PlayerbotAI::Update` wall time (scheduler); PerfMonitor (`.bot perf`) per action/behavior.
- Per map: sum of bot AI time per world tick, p50/p95/p99.
- World: `BotPerf` W_SESSIONS / W_MAPS / W_SCRIPTS split (`World/World.cpp` probes).
- Overhead check: the same cell with PerfMon off vs on; the difference must be <2% world diff.

## 5. Crash, stuck and navmesh detection

| Check | How | Pass |
|---|---|---|
| Crash | container restarts / core dumps; `Server.log` tail for `ASSERT`/`Crash` | 0 |
| Freeze | core freeze detector (`MaxCoreStuckTime`); world diff max | no freeze; max < 1 s outside startup |
| Stuck | `server.playerbots.stuck` lines (TRAVEL with <5 yd progress in 90 s) | <1 per ACTIVE bot-hour; none repeating on the same target (loop guard) |
| Fall/under-world | watchdog rescues; extra probe samples every simulated bot every 10 s: bot Z vs navmesh floor at (x,y) | 0 bots >10 yd under the floor for >10 s; rescues reported per tile |
| Bee-lines | counter of raw-point spline launches in BotMover | **0** |
| Navmesh coverage | NOPATH/INCOMPLETE counts per map/tile | tiles with >20% NOPATH are listed for mmaps regeneration (mmaps freshness vs 12.1.5 unverified) |
| Thread misuse | debug build: assert that THREADUNSAFE handlers run on the world thread and that bots touched from a map thread are on that map | 0 asserts |

## 6. Human-present checks (needs the user)

- Walk into a capital with 100 ACTIVE bots: client FPS acceptable; bots near you are ACTIVE, the rest THROTTLED; no bot visibly walks through walls or falls.
- `.bot pause all` → bots stop within 1 tick; `.bot resume all` → they resume.
