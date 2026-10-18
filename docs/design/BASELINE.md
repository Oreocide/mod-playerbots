# Playerbots: P0 baselines

Source: the scale-benchmark workstream (`/home/brandon/trinitycore-tracking/perf/FINDINGS_AND_PLAN.md`, Update 2/3, 2026-10-15). I re-ran nothing for this file: the user is playing on the live server, and a second session is creating characters on it. All numbers are single runs, ±15%, no human online, docker `MapUpdate.Threads = 1`.

## What exists (usable as the P0 baseline)

| Cell | Memory (RSS) | CPU | Tick p95 | Notes |
|---|---|---|---|---|
| Idle, 0 bots | 5.4 GiB | — | — | + ~3 GiB one time at the first login into the big continents |
| 200 bots, old default (no gates) | ~10-13.6 GiB | 89% of one core | 11.2 ms | `world.total` 817 ms/s |
| 200 bots, all gates, rotation off, PerfMon off (R1) | 10.0-10.4 GiB | 25% | — | |
| 750 bots, all gates, rotation off, PerfMon off (R2) | 9.4-10.1 GiB flat | 33% | 1.8 ms (PerfMon on: 54%) | all online 161 s after world up |
| 750 bots, all gates, rotation on (Q2/R3) | 13.4→14.2 GiB (Q2), 9.65→11.1 GiB (R3) | 64-65% | 1.8-3.6 ms | leaks ~1.9-5 MB/s: keep `BotActiveAlone = 0` |

Cost model: ~34.5 MB per loaded grid (31 MB navmesh tile) and ~0.5 MB per bot, so grids cost memory, not bots.

## Gate A/B at the production-like config (peer session, runs V2 vs V3 in `trinitycore-tracking/perf/runs/`, 2026-10-16)

1000 bots, `BotActiveAlone = 0`, module `SleepElevators` on, nobody online, identical except `SkipBotVisibilityGridLoad` + `FrozenTickMs`:

| | Both gates ON (V3) | Both OFF (V2) |
|---|---|---|
| Steady RSS | ~11.7-12.8 GiB | ~11.7-12.8 GiB (no difference; login guard at 15 GiB untouched) |
| `world.total` | 322 ms/s | 401 ms/s |
| Frozen `Player::Update` | 22 ms/s | 114 ms/s (the `FrozenTickMs` effect) |
| Tick p95 | 2.6 ms | 3.6 ms |
| Process CPU | 45% of a core | 54% of a core |

`SkipBotVisibilityGridLoad` does nothing measurable while `BotActiveAlone = 0` (frozen bots don't relocate). The only cost of dropping both is about +9 CPU points per 1000 bots, roughly +0.25 s of work per second at 3000. So P0b can drop both for 1000 bots; keep a **module-side per-map frozen tick** in the design for 3000.

## What is still missing (needs the user)

| Missing | Why | Needs |
|---|---|---|
| Baseline with `MapUpdate.Threads = 4` | exposes the home/anchor/store races fixed in P0, and is the real test for P0b | a worldserver restart with a changed conf: the user is playing |
| Saves/s with bots online | sizes the save-interval stretch (PERFORMANCE.md) | a run with real saves enabled |
| 1000-bot cell | scale target | the character creation the other session is running (target 3000) to finish |
| Human-present cells | the strict thresholds apply there | the user logged in |
| New module code on a live server | P0's module changes are built, not deployed | user approval to copy the binary into `trinity-server/bin` and restart |

Until then the thresholds in TEST_PLAN.md are measured against the table above.
