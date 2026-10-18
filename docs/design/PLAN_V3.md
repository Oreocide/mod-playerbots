# Playerbots v3: goals, memory, quests, reactions, battlegrounds

Plan only. Nothing here is implemented. v1/v2 docs stay the record of what is already built.

**Soak (2026-10-16, `FORCE=200 soak.sh 1200 1`): PASS on the crash bar.** 1200 s, 1000 bots logged in, max RSS 15,667 MiB, 0 crash/assert lines, binary kept, confs restored. It is not the 1-hour acceptance in ROADMAP.md, and it is not the in-game innkeeper test. Movement quality is not clean: 428 `fall rescue` lines and 87 `ground snap` lines (`trinitycore-tracking/perf/p0_soak/run_2026-10-16_1036/RESULT.txt`). Questing and BG pathing wait on reading those lines; they do not block emotes, chairs, or the goal table.

**Working decisions** (the defaults in the planning brief; say if one should change):

1. Near a human, bots level by playing (real quests). Far bots do not play the world out. On a long timer, if nobody is near, a dice roll usually moves them to another zone of their level and sometimes raises them one level. No ghost run, no abstract quest credit.
2. Six personas, fixed per bot: quester, crafter, pvper, socialite, explorer, idler.
3. Memory is a bounded ring per kind. It survives relog and is cleared when that bot is reset.
4. BG pilot is Warsong Gulch and Arathi Basin. Bots join only while a human is queued.

No new core edits. The allowed footprint stays the minimal kit in CORE_EXIT.md.

## Architecture

Town life stays the weighted state machine in `RpgStrategy` (`modules/mod-playerbots/src/Strategies/Rpg/RpgStrategy.cpp:90` `RandomChangeStatus`). Combat and commands stay the Engine. v3 adds three layers above the statuses, all in the module:

```
GoalStore (read-only snapshot) + persona multipliers
  -> one goal row
    -> an existing rpg status, a quest behavior, or a BG role
      -> MoveAlongMesh for any real walk
Memory (ring buffers) written on each outcome
BotStatusBoard copies goal, target, last result for .bot inspect and a future chat module
```

A goal does not replace the Engine. It chooses which status or combat strategy runs, then that code does the work.

**Near vs far.** Near means a human within `BotActiveAlone.Radius` (150 yd), or the bot is grouped, in an instance, or in a battleground (`PlayerbotAI.cpp:186-193`). Near bots play the goal out: mesh path, handlers, loot. Far bots are frozen. They store the same goal and advance it abstractly (time passes, XP trickles, the goal completes or fails on a timer). They do not call `MoveAlongMesh`, `LoadGrid`, or a loot handler. `Debug.ForceActive` stays a test switch.

**Read path.** `GoalStore` loads `playerbots_goal` and `playerbots_persona` at startup on the world thread. `.bot goal reload` builds a new snapshot and swaps it under a shared mutex. Map threads take a shared lock, copy the rows they need, and drop the lock before any path or handler. Same shape as the P0 home/anchor copies.

## Data model

World DB, module SQL `modules/mod-playerbots/data/sql/db-world/2026_10_16_00_playerbots_goal.sql`:

| Table | Columns |
|---|---|
| `playerbots_goal` | `id, name, kind, weight, min_level, max_level, faction, class_mask, race_mask, map, zone, profession, game_event, hour_from, hour_to, weekday_mask, duration_min, duration_max, cooldown_sec, max_per_zone, status, chat_line` |
| `playerbots_persona` | `persona, kind, multiplier` |

`kind` is a small enum implemented in C++: `town`, `quest_hub`, `grind`, `gather`, `fish`, `explore`, `bg`, `event`, `afk`, `logoff`. `status` names an existing rpg status when `kind=town` (innkeeper, vendor, repair, trainer). Five seed rows ship with phase 2: innkeeper, repair, a kill-quest hub, fishing, afk. More rows are data, not code.

Characters DB, `data/sql/db-characters/2026_10_16_00_playerbots_memory.sql`:

| Table | Columns |
|---|---|
| `playerbots_bot_persona` | `guid` PK, `persona` |
| `playerbots_memory` | `guid, kind, slot, key, payload, at` PK `(guid, kind, slot)` |

`kind` is `place`, `goal`, `quest`, `death`, `person`, `badspot`, `favorite`. Each kind has a fixed cap (16, except `person` 32). Slot 0 is newest. One characters write per outcome, through `PlayerbotRepository`'s mutex (`Db/PlayerbotRepository.cpp`, the P0 lock), flushed like the store is today. A bot reset deletes that guid's rows. Shared world memory (death heat per 128 yd cell, rare-seen) is in-process on the world thread and dies on restart. Persisting it is out of this plan.

Config, all default off: `Playerbots.Goal.Enable`, `Playerbots.Memory.Enable`, `Playerbots.Quest.Collect`, `Playerbots.Quest.Interact`, `Playerbots.Quest.Explore`, `Playerbots.Quest.AbstractXp`, `Playerbots.React.ReturnEmotes`, `Playerbots.React.Chairs`, `Playerbots.Bg.Objectives`.

Commands: `.bot goal reload`, `.bot inspect <bot>` (goal, persona, last 8 outcomes), `.bot rpg` unchanged.

## Threads

| Work | Thread | Lock |
|---|---|---|
| Goal snapshot load/reload, persona seed, abstract goal timers for frozen bots | world (`PlayerbotsScripts.cpp:61` `WorldScript::OnUpdate`) | exclusive on the snapshot swap; none on the timer |
| Reading the snapshot, `MoveAlongMesh`, INPLACE handlers | bot's map thread (AI tick) | shared lock, copy, unlock, then work |
| Memory flush, mail, AH, gossip select, loot, repop, reclaim corpse, battlefield port | world queue, GUID re-resolved at run time | queue mutex; repository mutex for the DB write |
| BG queue fill | world, already (`ActivityMgr::Scan` from the same `OnUpdate`) | existing ActivityMgr backoff maps |

INPLACE, safe on the map thread (H, `src/server/game/Server/Protocol/Opcodes.cpp`): `CMSG_GAME_OBJ_USE` :467, `CMSG_EMOTE` :455, `CMSG_SEND_TEXT_EMOTE` :1024, `CMSG_ATTACK_SWING` / `STOP` :174-175. THREADUNSAFE, world queue only (H, same file): loot :689-693, mail :696-702, gossip select :522, auction hello/browse :178-182, repop :956, reclaim corpse :947, stand-state :1095, battlemaster join :214, battlefield port :212.

`Player::GiveXP` for abstract XP runs on the bot's map thread inside the frozen AI branch. It does not run on the world thread (M: the opcode path for real XP is map-side; calling it off-thread is unproven and not planned).

## Features

### Goals and personas

Preconditions: snapshot loaded; bot has a persona row (assigned once at creation, hash of guid into the six names if the row is missing). Pick = weighted roll of rows matching level, faction, class, race, zone, hour, weekday, active `game_event`, cooldown, and `max_per_zone`. Persona multiplies `weight`. Far bots roll the same table and store the result. Failure: empty match list → today's `RandomChangeStatus` weights. Fallback: `Goal.Enable=0` leaves RpgStrategy as it is.

### Memory and status board

Preconditions: repository mutex (P0, soak did not crash under 4 map threads; that is crash evidence, not a proof of no race, M). On each goal end, push one ring entry. `BotStatusBoard` is a copy under its own mutex, written when the goal changes, read by `.bot inspect` and later chat. No map thread holds the board lock while pathing. Failure: DB write fails → keep the ring in memory and retry next outcome. Reset deletes rows.

### Quests

See QUEST_PLAN.md. Collect and interact need the world queue because loot is THREADUNSAFE. Explore is a mesh walk into an area trigger (`CMSG_AREA_TRIGGER` is INPLACE, `Opcodes.cpp:170`, H). Chains follow `RewardNextQuest` on the quest template. Out of scope: escorts, vehicles, scenarios, phase-heavy quests (`PhaseGuard` stays).

### Reactions

Return emotes: `PlayerScript::OnTextEmote` (`Scripting/ScriptMgr.h:754`, upstream, H) and `HandleTextEmoteOpcode` (INPLACE). The hook runs on the sender's map thread (M: the handler is INPLACE, the script call site was not re-read this pass). Reply only if the bot is on that map, alive, not in combat, and inside `GreetDistance`. One reply per sender per `GreetCooldown`.

Chairs: `GAMEOBJECT_TYPE_CHAIR` = 7 (`Miscellaneous/SharedDefines.h:3171`, H). Use `HandleGameObjectUseOpcode` (INPLACE) on the map thread. Do not call `HandleStandStateChangeOpcode` from the map thread (`CMSG_STAND_STATE_CHANGE` is THREADUNSAFE, :1095, H). Failure: no chair in 15 yd → idle standing, which matches the "stand rather than sit" rule when nothing is there.

NPC queue, day/night curve, and events are data on the goal table (`max_per_zone`, `hour_from`/`hour_to`, `game_event`), not new code. Hostile players, rares, duels, and boats are later phases in ROADMAP_V3.md. Boats are L because the route follower teleports between docks today; a real ride needs transport passengers and is not in the first six phases.

### Far bots: one dice roll

A bot with no human inside `BotActiveAlone.Radius`, and not in a group or battleground, does not path, loot, or craft. AzerothCore's idle pass is the model (`AC:Bot/RandomPlayerbotMgr.cpp:1509-1577`, H): every 1–5 hours (`MinRandomBotTeleportInterval` / `Max` 3600/18000) an idle bot is teleported to a level spot, a city, or a hotspot (`RandomTeleportForLevel`, `:1808`). A separate, rarer refresh rerolls gear at the current level (`Randomize`, `:2124`; default 2 hours to 14 days). `IncreaseLevel` (`:2146`) exists and is not what the regular timer calls.

v3 uses one roll on that teleport timer, and only when the bot is far:

- Usually relocate to a zone in the bot's current level band (existing hotspot / graveyard spots that `SpawnPolicy` already allows).
- A small chance: gain one level, up to `Random.MaxLevel` (90 on this server), then relocate to that band.
- The destination is refused when it is the other faction's district (below). A bot a human can see is never teleported.

The standing mix is weighted by band, not flat by level. `Random.MaxLevel` is 90, and the bands are the continents those bots already use. Every non-cap band has the same weight, 10. A bot that falls in a band gets a random level inside it, so the levels inside 1–19 or 85–89 are not a stack on the bottom of the band. Level 90 is a single level and is weighted 12, a step above each other band, because a live realm has more people at the cap than in any one leveling band. It is not a majority of the bots. The weights are config (`Playerbots.Population.Weight.<band>`), so 90 can be raised later without a code change.

| Band | Levels | Where | Weight |
|---|---|---|---|
| 1–19 | random in the band | Eastern Kingdoms, Kalimdor | 10 |
| 20–39 | random in the band | Eastern Kingdoms, Kalimdor | 10 |
| 40–59 | random in the band | Eastern Kingdoms, Kalimdor | 10 |
| 60–69 | random in the band | Outland | 10 |
| 70–79 | random in the band | Northrend | 10 |
| 80–84 | random in the band | Cataclysm, on the old continents | 10 |
| 85–89 | random in the band | Pandaria | 10 |
| 90 | the level itself | Pandaria and the cities | 12 |

World death matches AzerothCore, not a corpse run. `ProcessBot` marks a dead bot and revives it in 60–300 seconds by resurrecting and teleporting to a grind spot (`:1478-1498`, `Revive` `:1582`, conf `MinRandomBotReviveTime` 60 / `Max` 300). No `MoveAlongMesh` as a ghost. A battleground corpse is the exception and stays with the BG pilot: a match full of unreleased bodies is the thing a player sees.

### Crafting where people stand

AzerothCore's city craft is the action name `craft random item` on the RPG object the bot is already visiting (`AC:Ai/World/Rpg/Action/RpgSubActions.cpp:363`). v3 keeps that shape. A bot with blacksmithing (the crafter persona's pair is already chosen in `BotProfessions`) walks to a forge or anvil in the city it is in, spends a few seconds there, and makes one random recipe it knows and can afford the reagents for. The item is junk on purpose. The auction post that already exists (`PostAtAuctioneer`) sells it on a later visit. If no recipe can be made, the bot still stands and works at the forge for those few seconds. Frozen bots do not craft.

### Faction districts inside a sanctuary

Neutral sanctuaries stay legal for both teams (IDENTITY R5). A sanctuary that is split into a Horde side and an Alliance side is not neutral on those sides. AzerothCore's `AreaTable.team` is 0 for the Dalaran districts, so the module does not trust the area table (`AC:Mgr/Travel/TravelMgr.cpp:106-214`, H):

- `IsHostileFactionArea`: Alliance is blocked from Sunreaver's Sanctuary and the Sunreaver pavilion; Horde is blocked from the Silver Enclave and the Silver Covenant pavilion.
- `IsDalaranCoordinateHostileForTeam`: on Northrend (map 571), street boxes at city height, because the area reported on the approach is still the neutral city.
- `GetBankerTeam`: Dalaran tellers are forced to the side they stand on. The Underbelly teller stays neutral. Shattrath is a neutral capital in the same file (`:53`) and is not district-blocked.
- `EvictFromHostileFactionArea`: if the bot is already standing in the wrong district, teleport to another city spot on that map that fails the hostile test. Last resort on map 571 is a fixed point on the bot's own side. Called from the AI tick (`AC:Bot/PlayerbotAI.cpp:313`).
- The same test refuses a move destination (`NewRpgBaseAction.cpp:89`), an RPG target (`PossibleRpgTargetsValue.cpp:66`), and a teleport candidate (`RandomPlayerbotMgr.cpp:1686`).

`AreaTable` on build 12.1.0.69933 (`FactionGroupMask` 6 = both teams, `NoPvP` = sanctuary) still stores mask 0 on the faction quarters, so the list is explicit. Parent and child ids below are from that build.

| City | Parent area | Alliance side | Horde side |
|---|---|---|---|
| Dalaran, Northrend | 4395 | The Silver Enclave 4740 | Sunreaver's Sanctuary 4616 |
| Icecrown pavilions (the same two factions, outside the city) | 210 | Silver Covenant Pavilion 4667 (also 4677) | Sunreaver Pavilion 4666 (also 4676) |
| Dalaran, Broken Isles | 7502 | Greyfang Enclave 7508 | Windrunner's Sanctuary 7589 |
| Dalaran copy whose parent is 6611 | 6611 | The Silver Enclave 6627 | Sunreaver's Sanctuary 6634 |

Silvermoon City in Midnight (parent 15969, both teams, sanctuary) has no Alliance-only quarter. These five children are already `FactionGroupMask` 4, so the area table itself marks them Horde-only, unlike the Dalaran quarters above:

| Horde-only area | Id |
|---|---|
| Walk of Elders | 16083 |
| The Royal Exchange | 16084 |
| Court of Blood | 16085 |
| Court of the Sun | 16086 |
| Hall of Blood | 16618 |

An Alliance bot never paths into those five. A Horde bot may. The rest of that city (the Bazaar, Murder Row, Farstrider Square, the harbor, and the other mask-0 streets) stays open to both.

Streets in Northrend Dalaran still need a coordinate box when `GetAreaId` returns the parent city instead of the quarter, which is why AzerothCore boxed map 571. The Broken Isles city and Silvermoon get a box only after the same check fails there. Shattrath (3703) has Aldor Rise and Scryer's Tier, and AzerothCore leaves both open to both factions. Valdrakken's dragon enclaves, Dornogal, and Oribos have no faction quarter in this table, so both teams walk the whole city.

`HubIndex` picks, wander destinations, and the dice-roll teleport all use this list. A bot already in the wrong district is evicted to its own side of that city.

### Battlegrounds

See BG_PLAN.md. Fill stays human-gated in `ActivityMgr::FillBattlegrounds` (`ActivityMgr.cpp:466`). Objective reads use public `GameObject` and `Battleground` methods. The module does not subclass `BattlegroundScript` (those scripts are DB-bound, CORE_EXIT.md, H).

## Failure modes that cut across features

- No mesh path: count it, write a `badspot` memory, pick another goal. No straight-line spline (`MoveSplineInit.cpp:293-303`, G7).
- Handler returns a failure packet: log the reason, cooldown that NPC or node 10 min (same idea as the innkeeper unreachable block).
- World queue deeper than 256: drop the oldest service step, never a movement or combat step. Counter in the perf log.
- Persona or goal SQL missing: behave as the flags are off.
