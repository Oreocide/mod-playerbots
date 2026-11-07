# Playerbots: battleground plan (team planner)

Rewritten 2026-11-02 (phase 0 of the BG AI work). The old pilot plan (Warsong + Arathi only) is folded in where still true. Confidence per item: H high, M medium, L low.

Goal: a team planner per battleground instance and team, a seeded random attack/defend split, a data-driven objective graph for Alterac Valley and the bigger maps, a rally before boss pulls, then reactive play picked from the ranked list at the end. No core edits (CORE_EXIT.md), mmaps-only movement, every new behaviour behind a default-off `Playerbots.Bg.*` key.

## 1. Where things stand (verified 2026-11-02)

| Item | Finding | Conf |
|---|---|---|
| Pilot | `Strategies/GenericStrategies.cpp:1039-1388`: `PlayObjective`, `PlayWarsong`, `PlayCapturePoints`, `PlayEye`, `PathTo`, `SayBg`, `NearEnemy`, `FindGo`. `PvpAction` :1390, trigger "pvp zone" :1791 (no master, alive, in a BG/arena), strategy :1834, `Priority::Pvp` 30. Maps 2106, 726, 2107, 761, 566 | H |
| Pilot is greedy per bot | every bot searches the grid itself; defenders are `GetCounter() % 8 == 0` (:1251, capture maps only, same bots every match); WSG and EotS have no defenders; no team state, nothing rebalances | H |
| `FindGo` | 400 yd grid search per call, per bot, per tick (:1069) | H |
| `SayBg` | `Player::Say` (local /say) through `sWorldActionQueue`, 20 s per bot (:1119-1135) | H |
| Live key | `Playerbots.Bg.Objectives = 1` in `docker/etc/worldserver.conf.d/playerbots.conf:524`; dist `0` (`conf/playerbots.conf.dist:1069`) | H |
| Pilot captures | type 42 points are assaulted by calling `AssaultCapturePoint` directly: instant, no channel. Players cast 156098 "Capturing" (6 s, lock 2178) first | H |
| Thread of BG bots | world thread: `WorldScript::OnUpdate` -> `TickOtherMapBots` (`Script/PlayerbotsScripts.cpp:93`, `PlayerbotMgr.cpp:491-508`). `ForEachMapScript` never gives BG maps to a `WorldMapScript` (`ScriptMgr.cpp:1603-1612`) and `BattlegroundMapScript` is DB-bound. `World::Update` waits for the map threads (`MapManager.cpp:348`) before `OnWorldUpdate` (`World.cpp:2465`), so BG map state is quiescent while bots tick | H |
| Queue fill | `ActivityMgr::FillBattlegrounds` already fills to `GetMaxPlayersPerTeam` (`ActivityMgr.cpp:533`), 6 bots per 5 s scan, only queues a human is in, rated excluded. GROUP_CONTENT Gap A (stops at min) is out of date. What actually limits a match: core `CheckNormalMatch` stops picking at `minPlayers` per team (`BattlegroundQueue.cpp:658`), so AV starts at about 10 v 10; the module then stops because the human is invited (`ActivityMgr.cpp:517`). A started BG sits in the free-slot queue (`Battleground.cpp:955`) and queued players are invited into it (`BattlegroundQueue.cpp:776-800`) | H code, M runtime |
| Team sizes | BattlemasterList (the server's own `docker/data/dbc/enUS` file, build 70575, read with `trinitycore-tracking/tools/wdc5_read.py`, fields 9/10; tc-local 69933 agrees): AV 10-40, IoC 10-40, WSG classic 5-10, AB classic 8-15, EotS 8-15, Strand 8-15, Twin Peaks 5-10, Gilneas 3-10, Seething Shore 5-10; no `hotfixes.battlemaster_list` override | H |
| Score | pilot scripts score through `Battleground::AddPoint`, so `GetTeamScore` works: WSG/TP win at 3, AB/Gilneas/EotS at 1500 (`battleground_warsong_gulch.cpp:524`, `battleground_arathi_basin.cpp:221`, `battleground_battle_for_gilneas.cpp:129`, `battleground_eye_of_the_storm.cpp:293`). AV never calls it (reads 0); AV reinforcements are world states 3127 (A) / 3128 (H), start 700 | H |
| Core scripts | `world.battleground_scripts` maps 30, 566, 607, 628, 726, 727, 761, 998, 1803, 2106, 2107, 2656 (+ arenas) | H |
| Navmesh files | tiles + root in `docker/data/mmaps`: AV 28, IoC 57, Strand 68, Silvershard 6, Kotmogu 15, Seething Shore 71, Deephaul 18 (2106: 15, 2107: 49) | H files, path quality untested |
| Instance chat | `CHAT_MSG_INSTANCE_CHAT` calls the `OnPlayerChat` Group overload (`ChatHandler.cpp:466`, opcode THREADUNSAFE so world thread). The module overload (`PlayerbotsScripts.cpp:193`) serves only bots whose master is the speaker, so masterless BG bots ignore it today | H code, M runtime |
| Bots writing /bg | no helper; pattern: build the CMSG and call the session handler as `Strategies/Rpg/Ambient.cpp:153` does for emotes | M |
| Markers | 9100001-9100205 (`npc_bot_hotspot`, `SetVisible(false)`) are GM-placed, no spawn rows (`data/sql/db-world/2026_10_07_00_world_mod_playerbots_hotspots.sql`). No `playerbots_bg_node` table exists yet | H |
| AC reference | `BattleGroundTactics.cpp`: AV role code is `selectObjective` 1867-1923 (1207-1275 are the objective tables); roles are `urand(0,9)` per bot, never re-rolled in AV (3377); the only rally is a wander spot near the boss (2082-2101); everything is per bot; banner capture casts 21651 | H |

### Alterac Valley (map 30)

| Item | Finding | Conf |
|---|---|---|
| Nodes | 15 (`battleground_alterac_valley.cpp:216-235`): 0 Stormpike Aid Station, 1 Stormpike GY, 2 Stonehearth GY, 3 Snowfall GY, 4 Iceblood GY, 5 Frostwolf GY, 6 Frostwolf Relief Hut, 7 Dun Baldar South, 8 Dun Baldar North, 9 Icewing, 10 Stonehearth Bunker, 11 Iceblood Tower, 12 Tower Point, 13 Frostwolf East, 14 Frostwolf West | H |
| Node state | private `_nodes` in the script; readable from outside only as world states, `BGAVNodeInfo[]` (`:423-440`) gives per node A-control / A-assault / H-control / H-assault. A destroyed tower shows as the new owner's control id. Read with `Map::GetWorldStateValue` (`Map.h:848`) at any distance. Use the ids the table writes, not the enum names | H |
| Banners | type 1 buttons, lock 1479 (PvP open), `go_av_capturable_object` / `go_av_contested_object`; Snowfall 180418 is type 10. Path: cast 21651 "Opening" (8 s, OPEN_LOCK 17) -> `EffectOpenLock` -> `GameObject::Use(player, true)` -> `OnGossipHello` -> script assault or defend (`alterac_valley.cpp:143-155`, `battleground_alterac_valley.cpp:1022-1095`). No type 42 on map 30 | H |
| Assault timer | contested GO captures or destroys 4 min after the assault (`alterac_valley.cpp:165`); a click by the previous owner reverts it (`:1097-1133`) | H code |
| Score effects | player death -1; tower/bunker destroyed -75 to its team (`:815`); captain killed -100 (`:669-686`) and the captain's periodic team buff (23693 Balinda / 22751 Galvangar) stops; general killed ends the match (`:647-668`); a mine gives its owner +1 per 45 s; graveyards change no score, they move the respawn | H |
| Towers and generals | every destroyed tower/bunker despawns one marshal/warmaster: spawn groups 1116-1123 have a `conditions` row (type 33, world state of "destroyed" == 0). They stand within 6 yd of their general, so each one alive joins the general's pull. Nothing else gates a general | H |
| Bosses | Drek'Thar 11946 (-1371, -220, 98), Galvangar 11947 (-545, -165, 58), Vanndar 11948 (722, -11, 51), Balinda 11949 (-58, -287, 16); alive world states 601 / 1352 / 602 / 1351 | H |
| Leash | all four bosses and the marshals evade at > 50 yd 2D from home, checked every 5 s (`boss_balinda.cpp:154-164` incl. her water elemental, `boss_galvangar.cpp:78`, `boss_drekthar.cpp:77`, `boss_vanndar.cpp:70`, `alterac_valley.cpp:113-123`) | H |
| Mines | Irondeep (north) and Coldtooth (south); owner changes when the mine boss dies (Morloch 11657 / Snivvle 11677 while neutral, then Umi/Keetar, Agi/Masha); owner world states 801 / 804 | H |
| Spirit healer wave | `RESURRECTION_INTERVAL` (`Battleground.h:133`) is unused; the wave is the spirit healer channel 22011 or the per-player aura 156758 | M-L |
| Data defects | `BGAVNodeInfo` swaps Dun Baldar S/N assault ids (1374/1375) and the Iceblood/Tower Point owner ids (1187/1188); West Frostwolf Warmaster group 1123 has no creature and its comment says East; the first captain buff timer is `urand(0,4) * 60` ms (`:578`) where the repeat uses `* 60000`. Filed as issues, not blockers | H |

### Other scripted maps (phase 5)

Isle of Conquest 628 (nodes + gates, vehicles), Strand 607 (gates in rounds, vehicles), Silvershard 727 (carts), Kotmogu 998 (orbs, GO 212091-212094), Seething Shore 1803 (moving azerite nodes), Deephaul Ravine 2656 (carts + crystal). Each needs its own note first; IoC and Strand need the vehicle note. M.

## 2. Design

### Threads
- The planner ticks from a module `WorldScript::OnUpdate` (`playerbots_bg`, `src/Battleground/BgScripts.cpp`), world thread, after the map threads finish. Each instance re-plans every `Bg.Planner.TickMs` (3000), staggered by instance id.
- BG bots stay on the world thread (`TickOtherMapBots`) and read their assignment from the planner on that same thread. No locks. The chat hook (phase 4) runs there too.
- If BG bot ticks ever move to a map thread, the planner API needs a lock first (the rule is written at the top of `BgPlanner.h`).

### Planner (`src/Battleground/BgPlanner.*`)
- Registry keyed by `(mapId, instanceId)`: two team plans plus objective states. Bots touch it from their BG action; no touch for 30 s or `STATUS_WAIT_LEAVE` drops it. `Battleground*` is never stored: re-fetched each tick through `sMapMgr->FindMap(map, instance)`.
- Roster from `bg->GetBgMap()->GetPlayers()`: bots and humans, alive, spec role.
- Objective state: AV from world states; pilot maps from GO guids cached per match (`Map::GetGameObject`), with one pass over the map's object store, at most every 5 s, only when a guid misses (flags and respawned banners get new guids). Capture points are read from `GetGOValue()->CapturePoint` (state, last capturing team), towers from `GetControllingTeam`, flags from `GetFlagState`/`GetFlagCarrierGUID` and their `NEW_FLAG_DROP` object. Score from `GetTeamScore` or a world-state pair per map.
- Output per bot: job (attack, defend, carrier, escort, heal, rally), objective, point, radius, squad, since. Kept in the planner's own map keyed by `ObjectGuid`, never a `PlayerbotAI` member.
- Runs only with `Playerbots.Bg.Objectives = 1`, `Playerbots.Bg.Planner = 1` and the map in `Playerbots.Bg.Planner.Maps`; the startup log warns about Planner on with Objectives off.

### Random split (`src/Battleground/BgRules.h`, pure, unit-tested)
- At match start each team rolls `share` in `[Bg.DefendMinPct, Bg.DefendMaxPct]` (20-40), seeded from instance id, team and match start time. The teams roll separately. `.bot bg` prints the seed.
- Bots are ordered by `hash(seed, guid)`, a seeded shuffle. Defenders are the first `round(share x bots)`, spread over held objectives by weight; a team that holds nothing has no defenders that tick (the WSG flag room always counts).
- Reassign on: an objective changing hands, a flag picked up, dropped or back in base, a bot joining or leaving, a score swing (phase 1: the lead changes sign; a per-map score step comes with reactive #4), and every 60 s. Only the difference moves: bots whose `Bg.RoleHoldSeconds` (45) ran out, nearest to the new job first. A defender whose node fell is free at once. A bot that joins mid-match gets the job the team is short of.

### Objective graph (phase 3, world table `playerbots_bg_node`)
Built 2026-11-06 (`data/sql/db-world/2026_11_06_00_world_mod_playerbots_bg_node.sql`, read by `BgMapData.cpp LoadNodes` at startup and by `.bot bg reload`, which drops every plan because plans index the rows). Columns: `map, node, kind (capture | burn | kill_captain | kill_general | rally | gate), side, name, x, y, z, radius, creature_entry, ws_a_ctrl, ws_a_assault, ws_h_ctrl, ws_h_assault, ws_alive, requires, ally_weight, horde_weight, min_group, rally, leash_r, aliases, comment`. Against the phase 0 draft: `go_entry` is gone (the planner never searches for banners; a bot about to click looks for the banner within 8 yd of the row by its script, `go_av_capturable_object` / `go_av_contested_object`, in world, spawned and `GO_STATE_READY`: several banners share a spot and spawn groups keep the one of the node's state), `leash_x`/`leash_y` are gone (the leash centre is the boss's `GetHomePosition`), `side` is new (the owner at the start, a tower's builder, a boss's side, the team that stands at a rally or gate row), `take_mine`/`flag` wait for later maps, `gate` is new. `requires` is a soft gate: `"a,b"` = all, `"2:a,b,c"` = any two. One weight per team covers both uses: among nodes the team does not hold it is the attack order (highest first), among nodes it holds the share of defenders; 0 = never. Routes are not stored: bots path with `MoveAlongMesh` (old plan, still true).

State: all from the world states the script writes (`BgRules::ReadNode`: an assault keeps the previous owner; a tower held by the other side is destroyed; a boss is alive while `ws_alive` is 1), so Alterac Valley never runs `Discover`. Boss creatures are looked up by entry in one pass over the map's creatures, only while one is missing, at most every 5 s. Score: reinforcements (world states 3127/3128, start 3136).

Rally and gate rows (2026-11-06): rally spots are points of the AzerothCore mod-playerbots (GPL-2) AV road paths (`BattleGroundTactics.cpp`), each checked against `world.creature` on map 30 for creatures hostile to the gathering team within 30 yd (ownerless bots out of combat pull non-elite creatures within 30 yd, `NearbyEnemyRejection`): Balinda (Horde) -117 -264, 64 yd; Galvangar (Alliance) -445 -195, 104 yd (the AC point at 73 yd has a Frost Wolf hostile to the Alliance 5 yd away); Vanndar (Horde) 628 -101, 130 yd (closer points stand among the Stormpike Aid Station guards); Drek'Thar (Alliance) -1347 -287, 71 yd. All outside the 50 yd leash. Gates: the 180424 doors; during the preparation bots path to their door and stop 6-13 yd short (per bot, `BgRules::StopDistance`), on their own side. This is reactive #18 for Alterac Valley only (what the owner saw: bots standing at the spawn point); the other maps have no gate row and wait where they stand.

AV order, from what our script and DB do (Alliance attacking, Horde mirrored: Snowfall, Balinda, Stonehearth and Icewing bunkers, Stonehearth GY, Stormpike GY, Dun Baldar N/S, Aid Station, Vanndar):
1. Snowfall GY (neutral at start, shortens the run). M (tuning)
2. Galvangar in Iceblood Garrison: -100 Horde, ends the Horde buff. Rally outside the garrison, out of his aggro range. H effect, M position
3. Iceblood Tower and Tower Point: -75 each, each removes a warmaster from Drek'Thar's room. H
4. Iceblood GY, then Frostwolf GY: move the front. M
5. Frostwolf East and West towers: -75 each, warmasters gone. H
6. Frostwolf Relief Hut, the last graveyard. M
7. Drek'Thar: ends the match. Soft gate: 2 or more enemy towers down, or enemy reinforcements low, or match time over N minutes. Rally outside Frostwolf Keep. M (gate values)
Mines are a spare-bot job (reactive #13). Defenders keep our towers and graveyards and retake an assault inside its 4 min.

As built: the weights put that order in the rows (Alliance: Snowfall 9, Galvangar 8, Iceblood Tower and Tower Point 7, Iceblood GY 6, Frostwolf GY 5, the Frostwolf towers 4, Relief Hut 3, Drek'Thar 2; the Horde mirrored with Balinda, the bunkers, Stonehearth GY, Stormpike GY, Dun Baldar, Aid Station, Vanndar). One attack target at a time; two when 24 or more attackers and neither is a boss. A boss is a target once its rally row exists, the team has `min_group` players (captains 5, generals 10) and its `requires` are done: generals need 2 of the 4 enemy towers, or the soft gate (`Bg.SoftGateMinutes` 20 of match time, or the enemy reinforcements at `Bg.SoftGateScorePct` 40% of the start). Defend targets: our graveyards, our standing towers and our own running assaults; ours under assault weigh triple, attackers never turn back for them. A push that charged keeps its boss until it is dead or given up.

### Rally (`BgRules.h`, pure, unit-tested)
`Idle -> Gathering -> Charging -> Fighting -> (Regroup -> Gathering) | Done | Abandoned`
- Target = `min(floor, ceil(Bg.Rally.Pct% x living attackers on this push))`. Humans count unless they stand inside one of our defended objectives. Floors `Bg.Rally.GeneralFloor` 15, `Bg.Rally.CaptainFloor` 8; `Bg.Rally.Pct` 80.
- Present = living friendly players inside `Bg.Rally.Radius` (30) of the rally point. Charge at present >= target.
- `Bg.Rally.TimeoutSeconds` (120): with at least `Bg.Rally.TimeoutChargePct` (50)% of the target present, charge. Otherwise drop the push, pull defenders in and gather once more (`Bg.Rally.PullDefenders` 1, `Bg.Rally.MaxRestarts` 1), or pick another objective.
- Charging: one tank-role bot pulls, the rest follow 3 s later, healers hold at a ranged stand point (`Positioning::RangedStandPoint`).
- Fighting: no bot chases a target outside the boss leash circle (50 yd around home); nothing kites out of the building.
- Wipe (living on the push below `Bg.Rally.WipePct` 30% of those who charged, or the boss evaded): Regroup. All of the push, bots that resurrect included, walk back to the rally point; nobody runs in alone. Then a fresh gather.
- Done when the boss's alive world state reads 0.
- Tuning: `.bot bg rally set <node>` stores the GM's position in the row; `.bot bg show` summons temporary GM-only markers (5 min despawn). No permanent spawns, nothing always active, bots never need a marker.
- As built: the state machine is `BgRules::RallyStep` (unit tested); the planner feeds it once per plan (`Planner::StepRally`). The puller is the tank on the push nearest the boss, else the nearest melee, else the nearest ranged, never a healer. A boss that is out of combat 15 s after the charge reset; a regroup lasts at least 10 s and until the boss is out of combat. A push given up blocks that boss for the team for 2 minutes. `rally set` moves the row in memory and in the table and prints the UPDATE for the SQL file (a changed SQL file is applied again and replaces the table). Markers: creature 1 ("Waypoint (Only GM can see it)", the core's `.wp show` marker) as a private summon of the GM.
- Combat: once a bot is in combat `Play` no longer runs (the attack action, 60, beats pvp, 30; one action per tick), so the leash rule is its own trigger and action, `bg leash` at `Priority::Attack + 3` in the "pvp" strategy, combat only (`BgPlay.cpp`, registered by `Bg::Register`). Active while the bot's push charges or fights and its victim stands outside the leash circle or the bot is more than 10 yd outside it: the boss becomes the forced target (`Playerbots.Combat.RoleTargeting = 1` live puts the forced target first), a far victim is dropped, a bot outside walks back in. During a regroup it walks the bot to the rally point.

### Bot side (`src/Battleground/BgPlay.*`)
- One hook in `PlayObjective`, after the `BgObjectives` check: `if (Bg::Plays(bot)) return Bg::Play(bot, ai);`. With the key off the pilot runs exactly as today.
- Flags: lock-free flags (WSG, Twin Peaks) are picked up with `GameObject::Use`, as a click does; a dropped flag is its `NEW_FLAG_DROP` object (return or pick up). The Eye flag has lock 2178 and takes the 156098 cast. The pilot never picked a flag up.
- Movement only through `MoveAlongMesh`; `NoPath` writes `badspot` and the planner drops that node for that bot. Mounting through `UpdateMountForTravel`.
- Captures cast the real spell on the GO (156098 on type 42, 21651 on AV banners): damage interrupts, one capper per node, the others guard. M until seen in game (a server-side cast of a spell the bot does not know).
- Targets only inside the objective radius: enemy carrier, then enemies on our node, then the nearest. Never past a leash.

### Command
`.bot bg` (your battleground), `.bot bg list`, `.bot bg <instanceId>` (console): per team the seed and roll, job counts, current target, rally state (`gathering 9/12, 74 s left`) and objective states. `.bot bg roles` adds every bot's job, objective and job age. Phase 3 adds `rally set` and `show`.

### Talk (old plan, still the target)
Fixed phrase table, instance chat, at most one line per bot per 20 s and one per event per team. No model. Until reactive #9 is built the planner path says nothing; the pilot keeps its /say lines.

### Failure and fallback (old plan, still true)
- No mesh path to a node: `badspot`, move to the nearest node that does path. Never a raw `MoveTo`.
- Objective GO not found: fight inside 40 yd of the bot's position, log it, do not wander.
- Keys off: today's pilot or nearest-enemy behaviour.
- Death: release and mesh back from the spirit healer (in-place resurrect until the death phase lands).

## 3. Phases (each needs the owner's go)

| # | Scope | Keys (default) | Conf |
|---|---|---|---|
| 0 | This document, memory, issues | none | done 2026-11-02 |
| 1 | Planner and random split on the pilot maps, code tables, no SQL; real capture casts | `Bg.Planner` 0, `Bg.Planner.Maps` 2106,726,2107,761,566, `Bg.Planner.TickMs` 3000, `Bg.DefendMinPct` 20, `Bg.DefendMaxPct` 40, `Bg.RoleHoldSeconds` 45 | H design |
| 2 | Keep filling the running BG a human is in, up to its max: human-gated, rated and arenas excluded. Per team `need = cap - GetInvitedCount - queued` (the invited count already holds the players inside: it drops only on leave or a removed invite). The core invites from the queue into the free slots (`FillPlayersToBG`): during the preparation `GetFreeSlotsForTeam` is `max - invited` (`Battleground.InvitationType = 0`), after the gates open one per team per queue update. So at most 3 bots per team wait during the preparation and 2 after, and each scan calls `BattlegroundMgr::ScheduleQueueUpdate` while bots wait (a public call, the one the join handler makes). Bots waiting for a running match with a human inside are kept past the 60 s empty-queue grace | `Bg.FillToMax` 0, `Bg.FillMaxPerTeam` 0 (= the BG's max, never below its min) | built 2026-11-03, M runtime |
| 3 | AV: `playerbots_bg_node` SQL and loader, AV rows, rally state machine, boss pushes, the leash rule, the gate wait | `Bg.Rally.Pct` 80, `GeneralFloor` 15, `CaptainFloor` 8, `Radius` 30, `TimeoutSeconds` 120, `TimeoutChargePct` 50, `WipePct` 30, `PullDefenders` 1, `MaxRestarts` 1, `Bg.SoftGateMinutes` 20, `Bg.SoftGateScorePct` 40; 30 added to the default `Bg.Planner.Maps` (Planner itself stays 0) | built 2026-11-06, M positions |
| 4 | Reactive behaviours the owner picks from section 4 | one key each | - |
| 5 | Later maps, one note each, vehicles first | - | - |

Files (phase 1 as built): `src/Battleground/{BgRules.h, BgConfig.*, BgMapData.*, BgPlanner.*, BgPlay.*, BgScripts.cpp}`, `tests/bg_rules_test.cpp`. Phase 3 adds `data/sql/db-world/2026_11_06_00_world_mod_playerbots_bg_node.sql` and two lines in `GenericStrategies.cpp` (`Bg::Register(r)`, the `bg leash` node in the "pvp" strategy). The `Playerbots.Bg.*` keys live in `BgConfig` (loaded by the `playerbots_bg_world` script), not in `PlayerbotsConfig.h`, so no header layout changes; `.bot bg` is its own CommandScript whose `"bot bg"` entry the core merges into the `.bot` table, so `PlayerbotCommandScript.cpp` is untouched. Shared edits: `GenericStrategies.cpp` (include + hook), `playerbots_loader.cpp`, `conf/playerbots.conf.dist`, `tests/run_engine_test.sh`; phase 2: `ActivityMgr.cpp` (running-match fill in `FillBattlegrounds`, the grace exception in `ServeBots`). Several of these hold other sessions' uncommitted work: check ListAgents before each edit, splice hunks for the commit (`modules/CLAUDE.md`).

## 4. Reactive behaviours, ranked

Dev: S up to 1 day, M 2-3 days, L more. Run: added cost per planner tick (3 s, one instance, R = roster up to 80, N = nodes up to 15) or per bot tick. Planner work is timed under `.bot perf` as "bg planner".

| # | Behaviour | Dev | Run |
|---|---|---|---|
| 1 | Threat response: a held node turning assaulted pulls the nearest defenders and 2 attackers; enemies vs allies within 40 yd decide; give up a lost node instead of feeding 2 into 8 | M | R x N distance checks per planner tick (about 1.2k), none per bot |
| 2 | Retake timers: AV 4 min, AB/Gilneas capture time from the GO (`Data0` 60 s); defenders retake before it runs out | S after 1 | one timestamp per node |
| 3 | Squads of 3 or more: nobody heads to an objective alone; the leader paths, members follow | M | one leader lookup per bot; squad build O(R log R) per planner tick |
| 4 | Score and clock: turtle when ahead, all-in late when behind; Arathi/Gilneas hold 3, never chase a 5-cap | S | none |
| 5 | Graveyard wave: bots resurrected together leave together (wait up to 10 s or for 3) | S-M | O(R) per planner tick |
| 6 | Real capture casts and one capper per node | in phase 1 | one cast per capture |
| 7 | Focus fire: enemy flag carrier, then healers, then low health; assist a human's target; the squad leader raid-marks the enemy healer (`Group::SetTargetIcon`) | M | cheaper than today: bots read the planner's enemy list instead of a map scan each |
| 8 | Fall back at 3:1 within 40 yd and call for help | M | reuses #1 counts |
| 9 | /bg call-outs from a phrase table, rate limited | S | one packet per line |
| 10 | Human /bg calls ("inc LM 3", "help BS", "def stables", "everyone to Drek") send the nearest bots; node aliases from the table; the Group `OnChat` overload also serves masterless BG bots. Check in game first that a human's /bg line reaches the hook | M | parse per human line |
| 11 | Follow a human who leads: the nearest squad goes along with a human heading for an objective for 5 s | M | per human heading vs N per planner tick |
| 12 | Per-bot, per-match archetypes with weights (objective player, midfield brawler, turtle defender, ninja capper, healer who sticks to a group, AV road fighter); not every bot plays well | M | none (applied at assignment) |
| 13 | Spare bots take an AV mine in a small group while the front stalls | S-M | none |
| 14 | Flag carrier play: peel, healers follow the FC, the FC kites in our flag room during a standoff | M | one distance per bot near the FC |
| 15 | Endgame: once the result is certain, the losing side farms kills, the winning side pushes the general | S | none |
| 16 | Mount for long runs, dismount near fights (mostly exists) | S | none |
| 17 | Ninja caps: a stealth-class bot sneaks to an undefended enemy node | M | none |
| 18 | Prep phase: buff, mount, wait at the gate instead of standing idle | S | none |
| 19 | A bot that dies 3 times at one spot takes another lane | S | one counter per bot |
| 20 | Per-team skill weight so the human's side is neither always winning nor always losing | M | none |

Later, each with its own vehicle note first: IoC and Strand siege vehicles, Wintergrasp, Silvershard carts, Kotmogu orbs, Seething Shore moving nodes.

## 5. In-game test scripts

Before each install or restart: ask the owner, check `SELECT online FROM auth.account WHERE id=2`, look in `docker/etc/worldserver.conf.d/` for a PacketLogFile drop-in, `docker restart -t 120`.

Phase 1
1. `tests/run_engine_test.sh` passes (roll stays inside min-max, the two teams get different shuffle orders, the shuffle is not in guid order, the hold time holds, only the difference moves).
2. Conf: `Playerbots.Bg.Planner = 1` (Objectives already 1). Queue Warsong Gulch.
3. `.bot bg`: two seeds and two rolls (equal shares are possible, about 1 match in 21), job counts adding up to the bots per team. `.bot bg roles`: defenders in our flag room, the rest attacking or escorting.
4. A flag pickup, drop or capture shows as `last event` in `.bot bg`; while our flag carrier runs, `.bot bg roles` shows it as `carrier` and two attackers as `escort ... with <carrier>`. `last change` names the event that moved jobs; no bot changes job twice inside 45 s.
5. Queue Arathi Basin: defenders spread over held nodes; a capper stands 6 s casting "Capturing" and stops when hit, the others guard.
6. Second match: different rolls. `Playerbots.Bg.Planner = 0` + `.reload config`: the next match plays like the pilot.

Phase 2
1. With `Playerbots.Bg.FillToMax = 0` (the fill code path is the old one), queue AV and count each team after the gates open: `.bot bg` in the match prints a team size line (`.bot bg list` from the console). That is the baseline.
2. `Playerbots.Bg.FillToMax = 1` and `.reload config` (it wipes Server.log and DBErrors.log, copy them first). During the preparation the counts rise by up to 3 per team per 5 s scan; once the gates are open, by about 1 per team per scan to 40 v 40 (39 bots on your side), with no more than 2 per team `queued`. The two teams stay within a few of each other (the core's balance rule).
3. Record `.bot perf` (needs `Playerbots.PerfMon.Enable = 1`), RSS against `Playerbots.Random.MaxRssMB` (20000) and the world tick time at 80 active bots. Any bench-only conf is removed afterwards.
4. With nobody queued, no bot joins a BG queue.

Phase 3
0. Conf: add 30 to `Playerbots.Bg.Planner.Maps` in `docker/etc/worldserver.conf.d/playerbots.conf` (it has its own list). The startup log says `loaded 25 battleground objective rows`. This is the first match the planner plays at all (phase 1 was never played): `.bot bg` first, check `captures: cast N` rises and `used directly` stays low.
0b. During the preparation both teams walk to their gate and stand in front of it, spread out.
1. Queue AV. `.bot bg` per team: current target (for example Galvangar), the push line `gathering 9/12 at the rally point, 74 s left`, then `charging`, `fighting`; nodes and bosses lines below.
2. Watch a tank bot pull while healers stay back; Galvangar or Balinda never leave their 50 yd leash.
3. Wipe them once: the survivors and the resurrected walk back to the rally point together.
4. Towers fall before the Drek'Thar or Vanndar push; the general push gathers 15 or more.
5. `.bot bg rally set <node>` at a new spot, `.bot bg show`: the marker appears at the new spot, the next push gathers there.

## 6. Decisions

- 2026-11-06 (owner): AV bots standing at the spawn point get no stopgap; phase 3 builds the logic (go given the same day). The gate wait went into phase 3 for AV because that is what the owner saw.
- 2026-11-02 (owner): seed = instance id + team + match start time; humans count toward a rally unless they stand at a defended objective; rally tuning by GM command and temporary markers; planner bots cast the real capture spells, the pilot stays instant with the key off.
- Earlier (still standing): the module does not add a second BG script (DB-bound); roles are rewritten against TC getters, not copied (PROVENANCE.md); no straight-line fallback.
