# Playerbots v3: battleground plan

Pilot: Warsong Gulch and Arathi Basin, and only while a human is queued. Arenas and Wintergrasp are later (ROADMAP_V3 phase 10).

## Which maps are live

`battleground_scripts` on this database (queried 2026-10-16, H):

| Map | Script | mmap tiles in `docker/data/mmaps` |
|---|---|---|
| 2106 | `battleground_warsong_gulch` | 15 (`2106.mmap` + `2106_*.mmtile`) |
| 2107 | `battleground_arathi_basin` | 49 |

Classic maps still have tiles (489 Warsong 12, 529 Arathi 16, 566 Eye 24, 726 Twin Peaks 9, 761 Gilneas 14, 607 Strand 68, 628 Isle 57, 30 Alterac 28) but they are not the rows the server scripts. IDEAS.md counted those classic tiles. The pilot uses 2106 and 2107. Confidence H for the script table and the tile counts.

Wintergrasp is not a row in `battleground_scripts`. Map 2118 has 43 tiles; that is not a plan to implement it. It needs its own vehicle note first.

## What the module may call

`BattlegroundMapScript` is DB-bound, so the module cannot add a second script to these maps (CORE_EXIT.md, H). Read public state:

| Need | API | Cite | Conf |
|---|---|---|---|
| Capture owner | `GameObject::GetControllingTeam` | `Entities/GameObject/GameObject.h:465` | H |
| Capture interaction | `CanInteractWithCapturePoint`, `AssaultCapturePoint` | same file :443-445 | H public, M that a bot may call `AssaultCapturePoint` (it is what the GO does for a player; call it only from the map thread after a successful interact) |
| Flag state and carrier | `GameObject::GetFlagState`, `GetFlagCarrierGUID` | `GameObject.h:446-447` | H |
| Score | `Battleground::GetTeamScore` | `Battlegrounds/Battleground.h:434` | H |
| Queue join | already `ActivityMgr::FillBattlegrounds` on the world thread | `ActivityMgr.cpp:466`, called from `PlayerbotsScripts.cpp:75` | H |
| Port in | `CMSG_BATTLEFIELD_PORT` THREADUNSAFE | `Opcodes.cpp:212` | H, so the accept step is the world queue |

The WSG script's own `GetFlagState(TeamId)` (`scripts/Battlegrounds/WarsongGulch/battleground_warsong_gulch.cpp:279`) is on the script object. The module does not call it. It finds the flag GOs and uses the `GameObject` getters.

## Objective data

Arathi spawns are in the script as entries (H, `scripts/Battlegrounds/ArathiBasin/battleground_arathi_basin.cpp:116-120`):

| Node | Entry |
|---|---|
| Stables | 227420 |
| Blacksmith | 227522 |
| Farm | 227536 |
| Gold Mine | 227538 |
| Lumber Mill | 227544 |

The world `gameobject` table on map 2107 has 5 rows of type 42 (`GAMEOBJECT_TYPE_CAPTURE_POINT`, `SharedDefines.h:3206`). That matches the five nodes (H for the count, M that each row is one of those entries; confirm entries before seeding).

Warsong map 2106 has no type 24 (flagstand) or 26 (flagdrop) row in `gameobject`. The script owns the flags at runtime (`OnFlagStateChange` at `battleground_warsong_gulch.cpp:387`, H). The data table therefore stores the flag GO entries the script spawns, filled by reading the script once, not by the spawn table. Until those entries are written down, phase 8 cannot tell a carried flag from a base flag. That lookup is part of the phase, not a core change.

Per-map table `playerbots_bg_node` (world SQL, same file family as the goal table): `map, role_hint, go_entry, x, y, z, ally_weight, horde_weight`. Routes between nodes are not stored. A bot paths with `MoveAlongMesh` when it decides to go there. Hand-check one path per node pair in game; if `NoPath`, mark the node pair blocked in the table.

## Roles

Rebalance when a node or flag changes owner. Caps so one role cannot take every bot:

| Role | Warsong | Arathi |
|---|---|---|
| carrier | the bot who picks the enemy flag up; everyone else stops trying to pick it up | unused |
| escort | up to 2 bots within 30 yd of our carrier, on the mesh back to our flag room | unused |
| attack | everyone else, toward the enemy flag room, retarget if our flag is dropped | toward the nearest enemy-held node |
| defend | 1 bot in our flag room while our flag is in base | 1 bot on each node we hold, once we hold 3 |
| healer | if the spec heals, follow the carrier, else follow the nearest attack bot | follow the nearest attack bot |

Target priority: enemy carrier, then an enemy standing on our node or flag, then the nearest enemy within 40 yd of the bot's current objective. Do not chase past that. A healer targets the lowest-health friendly in 30 yd who is in the carrier's group of nearby bots.

This is the same split AC uses (attack / defend / carrier) and the code is written against the TC getters above, not copied (PROVENANCE.md).

## Queue policy

`FillBattlegrounds` already leaves the queue alone when no human is in it (`ActivityMgr.cpp:470-476`, H). Phase 8 changes the stop line from `GetMinPlayersPerTeam` (`:491`) to the template max, still only for a queue a human is in, still staggered by `MaxBgBotsPerScan`. Rated queues (`queueId.TeamSize`) stay excluded (`:475`).

Gate wait: after port, the bot paths to its team's start and does not cross the gate until `Battleground` reports the match in progress. The exact "in progress" getter needs a one-line confirm in `Battleground.h` at implementation time (M: not re-read this pass). If it is protected, wait on the gate GO's open state instead of adding a core accessor.

## Talk

A phrase table in the module, sent with the instance-chat handler (`CMSG_CHAT_MESSAGE_INSTANCE_CHAT` is THREADUNSAFE, `Opcodes.cpp:331`, H) via the world queue. Lines are fixed strings: "inc mid", "flag out", "need help at stables". No model, no learned text. At most one line per bot per 20 s.

## Failure and fallback

- No mesh path to a node: `badspot` memory, switch to defend the nearest node that does path. Never a raw `MoveTo`.
- Flag GO not found yet: attack the nearest enemy player inside 40 yd of the bot's start, and log it. Do not wander the map.
- `Bg.Objectives=0`: today's nearest-enemy behavior (`GenericStrategies.cpp:321-353`) stays.
- A bot dies: phase 7 release, then mesh back from the spirit healer. Until phase 7, use the current in-place resurrect and do not call phase 8 done.
