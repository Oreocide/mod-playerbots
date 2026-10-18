# Playerbots v3: test plan

Headless checks live in `trinitycore-tracking/perf/audits/` as new scripts. In-game checks need a human and are marked as such. Nothing here restarts the server on its own.

## Already recorded

Short soak `run_2026-10-16_1036`: PASS, 1200 s, 4 map threads, `Debug.ForceActive=200`, 1000 bots, max RSS 15,667 MiB, 0 crash/assert, 428 fall-rescue lines, 87 ground-snap lines. Conf files were restored. The 1-hour bar in ROADMAP.md was not run. Before phase 5, sample 20 of those fall-rescue lines and note the map and the call site. If they are the old height helpers (QuestTravel, RouteFollower, the post-teleport snap), they are known leftovers, not a v3 regression.

## Per phase

| Phase | Check | Pass |
|---|---|---|
| 1 | In game, one bot, emote and a chair | one reply, one sit, no second reply inside the cooldown |
| 2 | `audits/goal_pick.sh` (below) plus `.bot inspect` on a frozen bot | picks follow weights; frozen bot has 0 mesh launches for that goal |
| 3 | Relog one bot; reset a different bot | memory row count unchanged after relog; 0 rows after reset |
| 4 | `audits/world_queue.sh` | forced loot and forced repair succeed; a logged-out guid's step is dropped; 20 min soak adds 0 assert lines |
| 5 | In game, 20 collect attempts, human within 150 yd | ≥16 complete; failures logged; fall-rescue count in that area does not rise |
| 6 | One 3-quest chain; one frozen bot for 10 min with `AbstractXp=1` | chain order holds; that bot's map shows no extra grid load in the perf log |
| 7 | Kill one bot beside a human | ghost mesh path, resurrect at corpse, one death memory |
| 8 | Human queues Warsong, then Arathi | both teams reach max; the match ends on flag or nodes; 0 straight-line moves; no bot queued in the following 5 min with the human out of queue |

## Audit scripts to add

`goal_pick.sh` (read-only SQL plus a GM-less simulation is not available headless). Practical version: enable `Goal.Enable` on a throwaway conf, run 10 minutes with `ForceActive=10`, grep `.bot inspect` is not headless, so the module logs one line per pick (`server.playerbots` debug, already the pattern). The script counts kinds and compares them to weights within 30%. It restores the conf the way `soak.sh` does.

`world_queue.sh`: wraps the short soak (`FORCE=50 soak.sh 1200 1` is the user's call, not this script's) and greps `Server.log` for assert text, `queue drop`, and `loot fail`. Pass: 0 assert, drops only for logged-out guids.

`quest_coverage.sh`: re-runs the QUEST_PLAN.md queries. Pass: monster-only, plus-item, plus-go, plus-talk, plus-explore stay within 5% of 15429 / 11159 / 1229 / 341 / 336. A content import that moves them is a doc edit, not a code failure, once the new numbers are written down.

`bg_nodes.sh`: `SELECT map, type, COUNT(*) FROM gameobject g JOIN gameobject_template gt ON gt.entry=g.id WHERE g.map IN (2106,2107) GROUP BY 1,2`. Pass: map 2107 still has 5 type-42 rows. Map 2106 having 0 flagstand rows is expected; the script-spawned entries are checked in code review, not in this query.

Existing audits stay: `enemy_zone_audit.sh` (baseline 29 of 750) and `armor_audit.sh` (baseline 108 of 4,635). v3 does not change spawn or gear. Re-run them only if a phase touches `SpawnPolicy` or `GearFactory`.

## Soak additions

Any soak that runs with a v3 flag on records the same RESULT.txt fields as `soak.sh` plus: world-queue depth max, goals completed, goals failed, mesh `NoPath` count, loot steps, repop steps. RSS abort stays 18,000 MiB. `BotActiveAlone` stays 0 except `ForceActive` for the test. Do not enable `Bg.Objectives` on a no-human soak; the queue policy would correctly do nothing, and a forced queue would start matches with nobody to watch them.

## Crash and stuck

- Process exit or assert text: fail, roll the binary back (the soak script already does this).
- A bot with the same goal and the same position for 10 min while a human is within 150 yd: stuck. Log it. The goal cooldown must fire; if it does not, the phase fails.
- `fall rescue` or `ground snap` for a bot running a v3 behavior: fail that attempt. The pre-v3 baseline is the 428 / 87 from the short soak, not zero, so compare per behavior, not against zero for the whole server.
